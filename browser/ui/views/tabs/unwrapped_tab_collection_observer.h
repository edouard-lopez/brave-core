/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_BROWSER_UI_VIEWS_TABS_UNWRAPPED_TAB_COLLECTION_OBSERVER_H_
#define BRAVE_BROWSER_UI_VIEWS_TABS_UNWRAPPED_TAB_COLLECTION_OBSERVER_H_

class RootTabCollectionNode;
class TabStripModel;

// Hooks for RootTabCollectionNode, used in place of
// TabCollection::AddObserver() and RemoveObserver() of the root collection.
// The chromium_src override of RootTabCollectionNode forward-declares these
// instead of including this header, so keep the signatures in sync with it.
//
// While |model| has tree tabs, |root_node| is notified of the changes of the
// collection hierarchy with tree tab node collections unwrapped (see
// tree_tab_view_utils.h), so that RootTabCollectionNode doesn't know about
// tree tabs. Otherwise the changes are passed on as they are.
namespace tree_tab_view_utils {

void AddUnwrappedTabCollectionObserver(TabStripModel* model,
                                       RootTabCollectionNode* root_node);
void RemoveUnwrappedTabCollectionObserver(RootTabCollectionNode* root_node);

}  // namespace tree_tab_view_utils

#endif  // BRAVE_BROWSER_UI_VIEWS_TABS_UNWRAPPED_TAB_COLLECTION_OBSERVER_H_
