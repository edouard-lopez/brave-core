// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "chrome/browser/ui/views/tabs/common/root_tab_collection_node.h"

// Tree tab hooks. Defined in
// brave/browser/ui/views/tabs/unwrapped_tab_collection_observer.cc so that this
// upstream target doesn't depend on Brave targets.
namespace tree_tab_view_utils {
void AddUnwrappedTabCollectionObserver(TabStripModel* model,
                                       RootTabCollectionNode* root_node);
void RemoveUnwrappedTabCollectionObserver(RootTabCollectionNode* root_node);
}  // namespace tree_tab_view_utils

#include <chrome/browser/ui/views/tabs/common/root_tab_collection_node.cc>
