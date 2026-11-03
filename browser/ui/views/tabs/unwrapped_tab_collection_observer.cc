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
#include <variant>

#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "brave/browser/ui/views/tabs/tree_tab_view_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/tabs/common/root_tab_collection_node.h"
#include "chrome/browser/ui/views/tabs/common/tab_collection_node.h"
#include "components/tabs/public/tab_collection.h"
#include "components/tabs/public/tab_collection_observer.h"
#include "components/tabs/public/tab_collection_types.h"
#include "components/tabs/public/tab_interface.h"

namespace tree_tab_view_utils {

namespace {

// A tab or a collection.
using TabOrCollection = tabs::ConstChildPtr;
using OwnedTabOrCollection = tabs::Child;

std::optional<TabOrCollection> GetItemForHandle(
    const tabs::TabCollectionNodeHandle& handle) {
  if (const auto* collection_handle =
          std::get_if<tabs::TabCollectionHandle>(&handle)) {
    if (const tabs::TabCollection* collection = collection_handle->Get()) {
      return TabOrCollection(collection);
    }
    return std::nullopt;
  }
  if (const tabs::TabInterface* tab = std::get<tabs::TabHandle>(handle).Get()) {
    return TabOrCollection(tab);
  }
  return std::nullopt;
}

tabs::TabCollectionNodeHandle GetNodeHandleForItem(
    const TabOrCollection& item) {
  if (const auto* tab =
          std::get_if<tabs::ConstDanglingUntriagedTabInterface>(&item)) {
    return (*tab)->GetHandle();
  }
  return std::get<tabs::ConstDanglingUntriagedTabCollection>(item)->GetHandle();
}

TabOrCollection GetItemForChild(const OwnedTabOrCollection& child) {
  if (const auto* collection =
          std::get_if<std::unique_ptr<tabs::TabCollection>>(&child)) {
    return TabOrCollection(
        static_cast<const tabs::TabCollection*>(collection->get()));
  }
  return TabOrCollection(static_cast<const tabs::TabInterface*>(
      std::get<tabs::ScopedTab>(child).get()));
}

const tabs::TabCollection* GetCollection(const TabOrCollection& item) {
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

// Returns the index of the direct child within the parent node, if it exists.
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
// with tree tab node collections unwrapped.
//
// Brave's tree tab collection delegate notifies the changes of tree tab node
// collections as it does for the other collections, so they are mirrored
// without them.
class UnwrappedTabCollectionObserver : public tabs::TabCollectionObserver {
 public:
  UnwrappedTabCollectionObserver(TabStripModel* model,
                                 RootTabCollectionNode* root_node)
      : tab_strip_model_(model), root_node_(root_node) {
    tab_strip_model_->Root()->AddObserver(this);
  }
  UnwrappedTabCollectionObserver(const UnwrappedTabCollectionObserver&) =
      delete;
  UnwrappedTabCollectionObserver& operator=(
      const UnwrappedTabCollectionObserver&) = delete;
  ~UnwrappedTabCollectionObserver() override {
    tab_strip_model_->Root()->RemoveObserver(this);
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
          item && IsInTabStrip(tab_strip_model_->Root(), *item)) {
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
        HasTreeTabs(*tab_strip_model_->Root()) ||
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
  void PlaceUnwrapped(const TabOrCollection& item) {
    const tabs::TabCollection* collection = GetCollection(item);
    if (!IsTreeTabNode(collection)) {
      PlaceItem(item);
      return;
    }
    for (const OwnedTabOrCollection& child :
         GetUnwrappedChildren(*collection)) {
      PlaceItem(GetItemForChild(child));
    }
  }

  // Places |item| (a tab, group or split) in the view model under its
  // unwrapped parent, right after the item that comes before it.
  void PlaceItem(const TabOrCollection& item) {
    const tabs::TabCollection* parent = GetUnwrappedParent(item);
    if (!parent) {
      return;
    }
    TabCollectionNode* parent_node =
        root_node_->GetNodeForHandle(parent->GetHandle());
    if (!parent_node) {
      // e.g. A new group. Place it first, along with |item|.
      PlaceItem(TabOrCollection(parent));
      parent_node = root_node_->GetNodeForHandle(parent->GetHandle());
      if (!parent_node) {
        return;
      }
    }

    const tabs::TabCollectionNodeHandle handle = GetNodeHandleForItem(item);
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
                         const TabOrCollection& item) {
    for (auto predecessor = GetUnwrappedPredecessor(item); predecessor;
         predecessor = GetUnwrappedPredecessor(*predecessor)) {
      if (const auto index = GetIndexOfChild(
              parent_node, GetNodeHandleForItem(*predecessor))) {
        return *index + 1;
      }
    }
    return 0;
  }

  void AddNode(const TabCollectionNode& parent_node,
               const TabOrCollection& item,
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
        {GetCollectionHandle(parent_node), index}, {GetNodeHandleForItem(item)},
        /*insert_from_detached=*/!has_existing_tabs);
    if (has_existing_tabs) {
      for (const OwnedTabOrCollection& child :
           GetUnwrappedChildren(*collection)) {
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
      if (!item || !IsInTabStrip(tab_strip_model_->Root(), *item)) {
        RemoveNode(child.GetHandle());
        continue;
      }
      RemoveDetachedNodes(child);
      ++i;
    }
  }

  const raw_ptr<TabStripModel> tab_strip_model_;
  const raw_ptr<RootTabCollectionNode> root_node_;
  bool unwrapping_ = false;
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
