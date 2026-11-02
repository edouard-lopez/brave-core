/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/browser/ui/views/tabs/tree_tab_view_utils.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

#include "brave/browser/ui/tabs/brave_tab_strip_model.h"
#include "brave/browser/ui/tabs/tree_tab_model.h"
#include "brave/components/tabs/public/tree_tab_node.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_collection.h"
#include "components/tabs/public/tab_interface.h"

namespace tree_tab_view_utils {

namespace {

void AppendUnwrappedChildren(
    const tabs::TabCollection& collection,
    std::vector<std::reference_wrapper<const tabs::Child>>& out) {
  for (const auto& child : collection.GetChildren()) {
    const auto* child_collection =
        std::get_if<std::unique_ptr<tabs::TabCollection>>(&child);
    if (child_collection &&
        (*child_collection)->type() == tabs::TabCollection::Type::TREE_NODE) {
      AppendUnwrappedChildren(**child_collection, out);
    } else {
      out.push_back(std::cref(child));
    }
  }
}

// Returns the parent of |item| in the collection hierarchy.
const tabs::TabCollection* GetRawParent(const tabs::ConstChildPtr& item) {
  if (const auto* tab =
          std::get_if<tabs::ConstDanglingUntriagedTabInterface>(&item)) {
    return tab->get() ? tab->get()->GetParentCollection() : nullptr;
  }
  const tabs::TabCollection* collection =
      std::get<tabs::ConstDanglingUntriagedTabCollection>(item);
  return collection ? collection->GetParentCollection() : nullptr;
}

std::optional<size_t> GetRawIndex(const tabs::TabCollection& parent,
                                  const tabs::ConstChildPtr& item) {
  const auto& children = parent.GetChildren();
  for (size_t i = 0; i < children.size(); ++i) {
    if (const auto* child_collection =
            std::get_if<std::unique_ptr<tabs::TabCollection>>(&children[i])) {
      const auto* collection =
          std::get_if<tabs::ConstDanglingUntriagedTabCollection>(&item);
      if (collection && collection->get() == child_collection->get()) {
        return i;
      }
    } else {
      const auto* tab =
          std::get_if<tabs::ConstDanglingUntriagedTabInterface>(&item);
      if (tab &&
          tab->get() == static_cast<const tabs::TabInterface*>(
                            std::get<tabs::ScopedTab>(children[i]).get())) {
        return i;
      }
    }
  }
  return std::nullopt;
}

std::optional<tabs::ConstChildPtr> GetLastItemForView(
    const tabs::TabCollection& collection);

// Returns the last item that |child| is mirrored as in the view model.
template <typename Child>
std::optional<tabs::ConstChildPtr> GetLastItemOfChild(const Child& child) {
  if (const auto* child_collection =
          std::get_if<std::unique_ptr<tabs::TabCollection>>(&child)) {
    const tabs::TabCollection* ptr = child_collection->get();
    if (ptr->type() == tabs::TabCollection::Type::TREE_NODE) {
      return GetLastItemForView(*ptr);
    }
    return tabs::ConstChildPtr(ptr);
  }
  return tabs::ConstChildPtr(static_cast<const tabs::TabInterface*>(
      std::get<tabs::ScopedTab>(child).get()));
}

std::optional<tabs::ConstChildPtr> GetLastItemForView(
    const tabs::TabCollection& collection) {
  const auto& children = collection.GetChildren();
  for (size_t i = children.size(); i > 0; --i) {
    if (auto item = GetLastItemOfChild(children[i - 1])) {
      return item;
    }
  }
  return std::nullopt;
}

}  // namespace

std::vector<std::reference_wrapper<const tabs::Child>> GetUnwrappedChildren(
    const tabs::TabCollection& collection) {
  std::vector<std::reference_wrapper<const tabs::Child>> children;
  AppendUnwrappedChildren(collection, children);
  return children;
}

const tabs::TabCollection* GetUnwrappedParent(const tabs::ConstChildPtr& item) {
  const tabs::TabCollection* parent = GetRawParent(item);
  while (parent && parent->type() == tabs::TabCollection::Type::TREE_NODE) {
    parent = parent->GetParentCollection();
  }
  return parent;
}

std::optional<tabs::ConstChildPtr> GetUnwrappedPredecessor(
    const tabs::ConstChildPtr& item) {
  tabs::ConstChildPtr current = item;
  while (const tabs::TabCollection* parent = GetRawParent(current)) {
    const std::optional<size_t> index = GetRawIndex(*parent, current);
    if (!index) {
      return std::nullopt;
    }

    const auto& children = parent->GetChildren();
    for (size_t i = *index; i > 0; --i) {
      if (auto previous = GetLastItemOfChild(children[i - 1])) {
        return previous;
      }
    }

    if (parent->type() != tabs::TabCollection::Type::TREE_NODE) {
      return std::nullopt;
    }
    // The tree tab node is unwrapped. What comes before it comes before |item|
    // as well.
    current =
        tabs::ConstChildPtr(static_cast<const tabs::TabCollection*>(parent));
  }
  return std::nullopt;
}

bool IsInTabStrip(const tabs::TabCollection* root,
                  const tabs::ConstChildPtr& item) {
  for (const tabs::TabCollection* parent = GetRawParent(item); parent;
       parent = parent->GetParentCollection()) {
    if (parent == root) {
      return true;
    }
  }
  return false;
}

std::vector<const tabs::TabInterface*> GetTabsOfTreeTabChange(
    TabStripModel* model,
    const TreeTabChange& change) {
  if (change.type == TreeTabChange::kNodeWillBeDestroyed) {
    return {};
  }
  TreeTabModel* tree_model =
      static_cast<BraveTabStripModel*>(model)->tree_model();
  const tabs::TreeTabNode* node =
      tree_model ? tree_model->GetNode(change.id) : nullptr;
  return node ? node->GetTabs() : std::vector<const tabs::TabInterface*>();
}

}  // namespace tree_tab_view_utils
