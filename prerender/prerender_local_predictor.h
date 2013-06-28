// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PRERENDER_PRERENDER_LOCAL_PREDICTOR_H_
#define CHROME_BROWSER_PRERENDER_PRERENDER_LOCAL_PREDICTOR_H_

#include <vector>

#include "base/containers/hash_tables.h"
#include "base/memory/scoped_vector.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/browser/common/cancelable_request.h"
#include "chrome/browser/history/visit_database.h"
#include "googleurl/src/gurl.h"

class HistoryService;

namespace content {
class SessionStorageNamespace;
class WebContents;
}

namespace gfx {
class Size;
}

namespace prerender {

class PrerenderHandle;
class PrerenderManager;

// PrerenderLocalPredictor maintains local browsing history to make prerender
// predictions.
// At this point, the class is not actually creating prerenders, but just
// recording timing stats about the effect prerendering would have.
class PrerenderLocalPredictor : public history::VisitDatabaseObserver {
 public:
  struct LocalPredictorURLInfo;
  struct LocalPredictorURLLookupInfo;
  enum Event {
    EVENT_CONSTRUCTED = 0,
    EVENT_INIT_SCHEDULED = 1,
    EVENT_INIT_STARTED = 2,
    EVENT_INIT_FAILED_NO_HISTORY = 3,
    EVENT_INIT_SUCCEEDED = 4,
    EVENT_ADD_VISIT = 5,
    EVENT_ADD_VISIT_INITIALIZED = 6,
    EVENT_ADD_VISIT_PRERENDER_IDENTIFIED = 7,
    EVENT_ADD_VISIT_RELEVANT_TRANSITION = 8,
    EVENT_ADD_VISIT_IDENTIFIED_PRERENDER_CANDIDATE = 9,
    EVENT_ADD_VISIT_PRERENDERING = 10,
    EVENT_GOT_PRERENDER_URL = 11,
    EVENT_ERROR_NO_PRERENDER_URL_FOR_PLT = 12,
    EVENT_ADD_VISIT_PRERENDERING_EXTENDED = 13,
    EVENT_PRERENDER_URL_LOOKUP_RESULT = 14,
    EVENT_PRERENDER_URL_LOOKUP_RESULT_ROOT_PAGE = 15,
    EVENT_PRERENDER_URL_LOOKUP_RESULT_IS_HTTP = 16,
    EVENT_PRERENDER_URL_LOOKUP_RESULT_HAS_QUERY_STRING = 17,
    EVENT_PRERENDER_URL_LOOKUP_RESULT_CONTAINS_LOGOUT = 18,
    EVENT_PRERENDER_URL_LOOKUP_RESULT_CONTAINS_LOGIN = 19,
    EVENT_START_URL_LOOKUP = 20,
    EVENT_ADD_VISIT_NOT_ROOTPAGE = 21,
    EVENT_URL_WHITELIST_ERROR = 22,
    EVENT_URL_WHITELIST_OK = 23,
    EVENT_PRERENDER_URL_LOOKUP_RESULT_ON_WHITELIST = 24,
    EVENT_PRERENDER_URL_LOOKUP_RESULT_ON_WHITELIST_ROOT_PAGE = 25,
    EVENT_PRERENDER_URL_LOOKUP_RESULT_EXTENDED_ROOT_PAGE = 26,
    EVENT_PRERENDER_URL_LOOKUP_RESULT_ROOT_PAGE_HTTP = 27,
    EVENT_PRERENDER_URL_LOOKUP_FAILED = 28,
    EVENT_PRERENDER_URL_LOOKUP_NO_SOURCE_WEBCONTENTS_FOUND = 29,
    EVENT_PRERENDER_URL_LOOKUP_NO_LOGGED_IN_TABLE_FOUND = 30,
    EVENT_PRERENDER_URL_LOOKUP_ISSUING_LOGGED_IN_LOOKUP = 31,
    EVENT_CONTINUE_PRERENDER_CHECK_STARTED = 32,
    EVENT_CONTINUE_PRERENDER_CHECK_NO_URL = 33,
    EVENT_CONTINUE_PRERENDER_CHECK_PRIORITY_TOO_LOW = 34,
    EVENT_CONTINUE_PRERENDER_CHECK_URLS_IDENTICAL_BUT_FRAGMENT = 35,
    EVENT_CONTINUE_PRERENDER_CHECK_HTTPS = 36,
    EVENT_CONTINUE_PRERENDER_CHECK_ROOT_PAGE = 37,
    EVENT_CONTINUE_PRERENDER_CHECK_LOGOUT_URL = 38,
    EVENT_CONTINUE_PRERENDER_CHECK_LOGIN_URL = 39,
    EVENT_CONTINUE_PRERENDER_CHECK_NOT_LOGGED_IN = 40,
    EVENT_CONTINUE_PRERENDER_CHECK_FALLTHROUGH_NOT_PRERENDERING = 41,
    EVENT_CONTINUE_PRERENDER_CHECK_ISSUING_PRERENDER = 42,
    EVENT_ISSUING_PRERENDER = 43,
    EVENT_NO_PRERENDER_CANDIDATES = 44,
    EVENT_GOT_HISTORY_ISSUING_LOOKUP = 45,
    EVENT_TAB_HELPER_URL_SEEN = 46,
    EVENT_TAB_HELPER_URL_SEEN_MATCH = 47,
    EVENT_TAB_HELPER_URL_SEEN_NAMESPACE_MATCH = 48,
    EVENT_PRERENDER_URL_LOOKUP_MULTIPLE_SOURCE_WEBCONTENTS_FOUND = 49,
    EVENT_CONTINUE_PRERENDER_CHECK_ON_SIDE_EFFECT_FREE_WHITELIST = 50,
    EVENT_MAX_VALUE
  };

  // A PrerenderLocalPredictor is owned by the PrerenderManager specified
  // in the constructor.  It will be destoryed at the time its owning
  // PrerenderManager is destroyed.
  explicit PrerenderLocalPredictor(PrerenderManager* prerender_manager);
  virtual ~PrerenderLocalPredictor();

  void Shutdown();

  // history::VisitDatabaseObserver implementation
  virtual void OnAddVisit(const history::BriefVisitInfo& info) OVERRIDE;

  void OnGetInitialVisitHistory(
      scoped_ptr<std::vector<history::BriefVisitInfo> > visit_history);

  void OnPLTEventForURL(const GURL& url, base::TimeDelta page_load_time);

  void OnTabHelperURLSeen(const GURL& url, content::WebContents* web_contents);

 private:
  struct PrerenderProperties;
  HistoryService* GetHistoryIfExists() const;
  void Init();
  bool IsPrerenderStillValid(PrerenderProperties* prerender) const;
  bool DoesPrerenderMatchPLTRecord(PrerenderProperties* prerender,
                                   const GURL& url,
                                   base::TimeDelta plt) const;
  void RecordEvent(Event event) const;

  void OnLookupURL(scoped_ptr<LocalPredictorURLLookupInfo> info);

  // Returns an element of issued_prerenders_, which should be replaced
  // by a new prerender of the priority indicated, or NULL, if the priority
  // is too low.
  PrerenderProperties* GetIssuedPrerenderSlotForPriority(double priority);

  void ContinuePrerenderCheck(
      scoped_refptr<content::SessionStorageNamespace> session_storage_namespace,
      scoped_ptr<gfx::Size> size,
      scoped_ptr<LocalPredictorURLLookupInfo> info);
  void LogCandidateURLStats(const GURL& url) const;
  void IssuePrerender(scoped_refptr<content::SessionStorageNamespace>
                      session_storage_namespace,
                      scoped_ptr<gfx::Size> size,
                      scoped_ptr<LocalPredictorURLInfo> info,
                      PrerenderProperties* prerender_properties);
  PrerenderManager* prerender_manager_;
  base::OneShotTimer<PrerenderLocalPredictor> timer_;

  // Delay after which to initialize, to avoid putting to much load on the
  // database thread early on when Chrome is starting up.
  static const int kInitDelayMs = 5 * 1000;

  // Whether we're registered with the history service as a
  // history::VisitDatabaseObserver.
  bool is_visit_database_observer_;

  CancelableRequestConsumer history_db_consumer_;

  scoped_ptr<std::vector<history::BriefVisitInfo> > visit_history_;

  scoped_ptr<PrerenderProperties> current_prerender_;
  scoped_ptr<PrerenderProperties> last_swapped_in_prerender_;

  ScopedVector<PrerenderProperties> issued_prerenders_;

  base::hash_set<int64> url_whitelist_;

  base::WeakPtrFactory<PrerenderLocalPredictor> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(PrerenderLocalPredictor);
};

}  // namespace prerender

#endif  // CHROME_BROWSER_PRERENDER_PRERENDER_LOCAL_PREDICTOR_H_
