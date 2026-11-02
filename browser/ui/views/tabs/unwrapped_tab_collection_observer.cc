/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/browser/ui/views/tabs/unwrapped_tab_collection_observer.h"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "brave/browser/ui/views/tabs/tree_tab_view_utils.h"
#include "brave/components/tabs/public/tree_tab_node_tab_collection.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/views/tabs/common/root_tab_collection_node.h"
#include "chrome/browser/ui/views/tabs/common/tab_collection_node.h"
#include "chrome/browser/ui/views/tabs/common/tab_view.h"
#include "components/tabs/public/tab_collection.h"
#include "components/tabs/public/tab_collection_observer.h"
#include "components/tabs/public/tab_collection_types.h"
#include "components/tabs/public/tab_interface.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"

namespace tree_tab_view_utils {

namespace {

// A tab or a collection.
using Item = tabs::ConstChildPtr;

std::optional<Item> GetItemForHandle(
    const tabs::TabCollectionNodeHandle& handle) {
  if (const auto* collection_handle =
          std::get_if<tabs::TabCollectionHandle>(&handle)) {
    if (const tabs::TabCollection* collection = collection_handle->Get()) {
      return Item(collection);
    }
    return std::nullopt;
  }
  if (const tabs::TabInterface* tab = std::get<tabs::TabHandle>(handle).Get()) {
    return Item(tab);
  }
  return std::nullopt;
}

tabs::TabCollectionNodeHandle GetHandleForItem(const Item& item) {
  if (const auto* tab =
          std::get_if<tabs::ConstDanglingUntriagedTabInterface>(&item)) {
    return (*tab)->GetHandle();
  }
  return std::get<tabs::ConstDanglingUntriagedTabCollection>(item)->GetHandle();
}

Item GetItemForChild(const tabs::Child& child) {
  if (const auto* collection =
          std::get_if<std::unique_ptr<tabs::TabCollection>>(&child)) {
    return Item(static_cast<const tabs::TabCollection*>(collection->get()));
  }
  return Item(static_cast<const tabs::TabInterface*>(
      std::get<tabs::ScopedTab>(child).get()));
}

const tabs::TabCollection* GetCollection(const Item& item) {
  const auto* collection =
      std::get_if<tabs::ConstDanglingUntriagedTabCollection>(&item);
  return collection ? collection->get() : nullptr;
}

bool IsTreeTabNode(const tabs::TabCollection* collection) {
  return collection &&
         collection->type() == tabs::TabCollection::Type::TREE_NODE;
}

// Returns true if the unpinned collection has tree tab node collections, in
// which case all of its children are.
bool HasTreeTabs(const tabs::TabCollection& root) {
  for (const auto& child : root.GetChildren()) {
    const auto* collection =
        std::get_if<std::unique_ptr<tabs::TabCollection>>(&child);
    if (!collection ||
        (*collection)->type() != tabs::TabCollection::Type::UNPINNED) {
      continue;
    }
    const auto& unpinned_children = (*collection)->GetChildren();
    if (unpinned_children.empty()) {
      return false;
    }
    const auto* first = std::get_if<std::unique_ptr<tabs::TabCollection>>(
        &unpinned_children.front());
    return first && IsTreeTabNode(first->get());
  }
  return false;
}

std::optional<size_t> GetIndexOfChild(
    const TabCollectionNode& parent_node,
    const tabs::TabCollectionNodeHandle& handle) {
  const auto& children = parent_node.children();
  for (size_t i = 0; i < children.size(); ++i) {
    if (children[i]->GetHandle() == handle) {
      return i;
    }
  }
  return std::nullopt;
}

tabs::TabCollectionHandle GetCollectionHandle(
    const TabCollectionNode& collection_node) {
  return std::get<tabs::TabCollectionHandle>(collection_node.GetHandle());
}

// Passes the changes of the collection hierarchy on to RootTabCollectionNode
// with tree tab node collections unwrapped. As the root collection sends them
// once the tab strip model is updated, they are applied to the view model by
// placing the tabs and collections that they are about where they are in the
// unwrapped hierarchy now, rather than by the positions that they carry.
//
// Brave's tree tab collection delegate moves tree tab node collections without
// any collection notification, which is reported by OnTreeTabChanged() only.
// The tabs under them are placed once the update is done.
class UnwrappedTabCollectionObserver : public tabs::TabCollectionObserver,
                                       public TabStripModelObserver {
 public:
  UnwrappedTabCollectionObserver(TabStripModel* model,
                                 RootTabCollectionNode* root_node)
      : model_(model), root_node_(root_node) {
    model_->Root()->AddObserver(this);
    model_->AddObserver(this);
  }
  UnwrappedTabCollectionObserver(const UnwrappedTabCollectionObserver&) =
      delete;
  UnwrappedTabCollectionObserver& operator=(
      const UnwrappedTabCollectionObserver&) = delete;
  ~UnwrappedTabCollectionObserver() override {
    model_->RemoveObserver(this);
    model_->Root()->RemoveObserver(this);
  }

  // tabs::TabCollectionObserver:
  void OnChildrenAdded(const tabs::TabCollection::Position& position,
                       const tabs::TabCollectionNodes& handles,
                       bool insert_from_detached) override {
    if (!ShouldUnwrap({position.parent_handle}, handles)) {
      root_observer()->OnChildrenAdded(position, handles, insert_from_detached);
      return;
    }
    for (const auto& handle : handles) {
      if (const auto item = GetItemForHandle(handle)) {
        PlaceUnwrapped(*item);
      }
    }
  }

  void OnChildrenRemoved(const tabs::TabCollection::Position& position,
                         const tabs::TabCollectionNodes& handles) override {
    if (!ShouldUnwrap({position.parent_handle}, handles)) {
      root_observer()->OnChildrenRemoved(position, handles);
      return;
    }
    bool remove_detached_nodes = false;
    for (const auto& handle : handles) {
      if (!root_node_->GetNodeForHandle(handle)) {
        // e.g. A tree tab node collection. Its descendants could have been
        // removed along with it.
        remove_detached_nodes = true;
        continue;
      }
      if (const auto item = GetItemForHandle(handle);
          item && IsInTabStrip(model_->Root(), *item)) {
        // Added back in the same update.
        PlaceItem(*item);
        continue;
      }
      RemoveNode(handle);
    }
    if (remove_detached_nodes) {
      RemoveDetachedNodes(*root_node_);
    }
  }

  void OnChildMoved(const tabs::TabCollection::Position& to_position,
                    const NodeData& node_data) override {
    if (!ShouldUnwrap(
            {node_data.position.parent_handle, to_position.parent_handle},
            {node_data.handle})) {
      root_observer()->OnChildMoved(to_position, node_data);
      return;
    }
    if (const auto item = GetItemForHandle(node_data.handle)) {
      PlaceUnwrapped(*item);
    }
  }

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override {
    if (selection.active_tab_changed()) {
      ScheduleExpandActiveTabAncestors();
    }
  }

  void OnTreeTabChanged(const TreeTabChange& change) override {
    const std::vector<const tabs::TabInterface*> node_tabs =
        GetTabsOfTreeTabChange(model_, change);
    for (const tabs::TabInterface* tab : node_tabs) {
      toggle_button_tabs_.push_back(tab->GetHandle());
    }

    if ((change.type == TreeTabChange::kNodeCreated ||
         change.type == TreeTabChange::kNodeReparented) &&
        !node_tabs.empty()) {
      // The tree tab node could have been moved along with its descendants.
      if (const auto* tree_node_collection =
              tabs::TreeTabNodeTabCollection::GetNearestTreeTabNodeCollection(
                  node_tabs.front())) {
        for (const tabs::TabInterface* tab :
             tree_node_collection->GetTabsRecursive()) {
          moved_tabs_.push_back(tab->GetHandle());
        }
      }
    }

    if (change.type == TreeTabChange::kNodeCollapsedStateChanged ||
        change.type == TreeTabChange::kNodeChildrenChanged ||
        change.type == TreeTabChange::kNodeReparented) {
      // Indentation and visibility.
      if (TabCollectionNode* unpinned_node =
              root_node_->GetChildNodeOfType(TabCollectionNode::Type::UNPINNED);
          unpinned_node && unpinned_node->view()) {
        unpinned_node->view()->InvalidateLayout();
      }
    }

    // The hierarchy is in the middle of the update.
    ScheduleUpdate();
  }

 private:
  tabs::TabCollectionObserver* root_observer() { return root_node_.get(); }

  // Returns true if the changes should be unwrapped. Latched, as the view model
  // has the unwrapped hierarchy from then on, e.g. even after the last tree
  // tab node collection is removed.
  bool ShouldUnwrap(
      std::initializer_list<tabs::TabCollectionHandle> parent_handles,
      const tabs::TabCollectionNodes& handles) {
    if (unwrapping_) {
      return true;
    }
    unwrapping_ =
        HasTreeTabs(*model_->Root()) ||
        std::ranges::any_of(parent_handles,
                            [&](const tabs::TabCollectionHandle& handle) {
                              return !root_node_->GetNodeForHandle(handle);
                            }) ||
        std::ranges::any_of(handles, [](const auto& handle) {
          const auto item = GetItemForHandle(handle);
          return item && IsTreeTabNode(GetCollection(*item));
        });
    return unwrapping_;
  }

  // Places |item|, or its unwrapped children if it's a tree tab node
  // collection.
  void PlaceUnwrapped(const Item& item) {
    const tabs::TabCollection* collection = GetCollection(item);
    if (!IsTreeTabNode(collection)) {
      PlaceItem(item);
      return;
    }
    for (const tabs::Child& child : GetUnwrappedChildren(*collection)) {
      PlaceItem(GetItemForChild(child));
    }
  }

  // Places |item| (a tab, group or split) in the view model under its
  // unwrapped parent, right after the item that comes before it.
  void PlaceItem(const Item& item) {
    const tabs::TabCollection* parent = GetUnwrappedParent(item);
    if (!parent) {
      return;
    }
    TabCollectionNode* parent_node =
        root_node_->GetNodeForHandle(parent->GetHandle());
    if (!parent_node) {
      // e.g. A new group. Place it first, along with |item|.
      PlaceItem(Item(parent));
      parent_node = root_node_->GetNodeForHandle(parent->GetHandle());
      if (!parent_node) {
        return;
      }
    }

    const tabs::TabCollectionNodeHandle handle = GetHandleForItem(item);
    size_t index = GetIndexToPlace(*parent_node, item);
    TabCollectionNode* current_parent_node =
        root_node_->GetParentNodeForHandle(handle);
    if (!current_parent_node) {
      AddNode(*parent_node, item, index);
      return;
    }

    const size_t current_index =
        GetIndexOfChild(*current_parent_node, handle).value();
    if (current_parent_node == parent_node) {
      if (current_index < index) {
        // It's removed from before |index| first.
        --index;
      }
      if (current_index == index) {
        return;
      }
    }
    root_observer()->OnChildMoved(
        {GetCollectionHandle(*parent_node), index},
        NodeData{{GetCollectionHandle(*current_parent_node), current_index},
                 handle});
  }

  // Returns the index right after the item before |item| in |parent_node|.
  size_t GetIndexToPlace(const TabCollectionNode& parent_node,
                         const Item& item) {
    for (auto predecessor = GetUnwrappedPredecessor(item); predecessor;
         predecessor = GetUnwrappedPredecessor(*predecessor)) {
      if (const auto index =
              GetIndexOfChild(parent_node, GetHandleForItem(*predecessor))) {
        return *index + 1;
      }
    }
    return 0;
  }

  void AddNode(const TabCollectionNode& parent_node,
               const Item& item,
               size_t index) {
    // Read the whole subtree, as Chromium does for detached collections, unless
    // some tabs under it are in the view model already. Those are moved into it
    // instead.
    const tabs::TabCollection* collection = GetCollection(item);
    const bool has_existing_tabs =
        collection &&
        std::ranges::any_of(
            collection->GetTabsRecursive(), [&](const tabs::TabInterface* tab) {
              return root_node_->GetNodeForHandle(tab->GetHandle());
            });
    root_observer()->OnChildrenAdded(
        {GetCollectionHandle(parent_node), index}, {GetHandleForItem(item)},
        /*insert_from_detached=*/!has_existing_tabs);
    if (has_existing_tabs) {
      for (const tabs::Child& child : GetUnwrappedChildren(*collection)) {
        PlaceItem(GetItemForChild(child));
      }
    }
  }

  void RemoveNode(const tabs::TabCollectionNodeHandle& handle) {
    TabCollectionNode* parent_node = root_node_->GetParentNodeForHandle(handle);
    if (!parent_node) {
      return;
    }
    root_observer()->OnChildrenRemoved(
        {GetCollectionHandle(*parent_node),
         GetIndexOfChild(*parent_node, handle).value()},
        {handle});
  }

  // Removes the nodes under |node| whose tabs or collections are destroyed or
  // not in the tab strip anymore.
  void RemoveDetachedNodes(TabCollectionNode& node) {
    for (size_t i = 0; i < node.children().size();) {
      TabCollectionNode& child = *node.children()[i];
      const auto item = GetItemForHandle(child.GetHandle());
      if (!item || !IsInTabStrip(model_->Root(), *item)) {
        RemoveNode(child.GetHandle());
        continue;
      }
      RemoveDetachedNodes(child);
      ++i;
    }
  }

  void ScheduleUpdate() {
    if (update_pending_) {
      return;
    }
    update_pending_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&UnwrappedTabCollectionObserver::Update,
                                  weak_ptr_factory_.GetWeakPtr()));
  }

  void Update() {
    update_pending_ = false;
    auto moved_tabs = std::exchange(moved_tabs_, {});
    auto toggle_button_tabs = std::exchange(toggle_button_tabs_, {});
    if (!root_node_->view() || model_->closing_all()) {
      return;
    }

    // Place them in tab strip model order, so that the item before each one is
    // placed already.
    std::vector<const tabs::TabInterface*> tabs;
    for (const auto& handle : moved_tabs) {
      const tabs::TabInterface* tab = handle.Get();
      if (tab && IsInTabStrip(model_->Root(), Item(tab))) {
        tabs.push_back(tab);
      }
    }
    std::ranges::sort(tabs, {}, [&](const tabs::TabInterface* tab) {
      return model_->GetIndexOfTab(tab);
    });
    for (const tabs::TabInterface* tab : tabs) {
      PlaceItem(Item(tab));
    }
    if (!tabs.empty()) {
      RemoveDetachedNodes(*root_node_);
    }

    for (const auto& handle : toggle_button_tabs) {
      if (TabCollectionNode* node = root_node_->GetNodeForHandle(handle)) {
        if (auto* tab_view = views::AsViewClass<TabView>(node->view())) {
          tab_view->UpdateTreeToggleButton();
        }
      }
    }

    // e.g. A new active tab could be moved under a collapsed tree tab node.
    ScheduleExpandActiveTabAncestors();
  }

  void ScheduleExpandActiveTabAncestors() {
    // Expanding notifies the tab strip model's observers, so don't do it in the
    // middle of the model's notifications.
    if (expand_pending_) {
      return;
    }
    expand_pending_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            &UnwrappedTabCollectionObserver::ExpandActiveTabAncestors,
            weak_ptr_factory_.GetWeakPtr()));
  }

  void ExpandActiveTabAncestors() {
    expand_pending_ = false;
    if (!model_->closing_all()) {
      ExpandCollapsedTreeTabAncestors(model_->GetActiveTab());
    }
  }

  const raw_ptr<TabStripModel> model_;
  const raw_ptr<RootTabCollectionNode> root_node_;
  bool unwrapping_ = false;
  std::vector<tabs::TabHandle> moved_tabs_;
  std::vector<tabs::TabHandle> toggle_button_tabs_;
  bool update_pending_ = false;
  bool expand_pending_ = false;
  base::WeakPtrFactory<UnwrappedTabCollectionObserver> weak_ptr_factory_{this};
};

std::map<RootTabCollectionNode*,
         std::unique_ptr<UnwrappedTabCollectionObserver>>&
GetObservers() {
  static base::NoDestructor<std::map<
      RootTabCollectionNode*, std::unique_ptr<UnwrappedTabCollectionObserver>>>
      observers;
  return *observers;
}

}  // namespace

void AddUnwrappedTabCollectionObserver(TabStripModel* model,
                                       RootTabCollectionNode* root_node) {
  auto& observer = GetObservers()[root_node];
  CHECK(!observer);
  observer = std::make_unique<UnwrappedTabCollectionObserver>(model, root_node);
}

void RemoveUnwrappedTabCollectionObserver(RootTabCollectionNode* root_node) {
  GetObservers().erase(root_node);
}

}  // namespace tree_tab_view_utils
