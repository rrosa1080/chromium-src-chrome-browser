// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/incident_reporting_service.h"

#include <map>
#include <string>

#include "base/bind.h"
#include "base/callback.h"
#include "base/lazy_instance.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/test_simple_task_runner.h"
#include "base/thread_task_runner_handle.h"
#include "base/threading/thread_local.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/browser/safe_browsing/incident_report_uploader.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/safe_browsing/csd.pb.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_pref_service_syncable.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/testing_profile_manager.h"
#include "net/url_request/url_request_context_getter.h"
#include "testing/gtest/include/gtest/gtest.h"

// A test fixture that sets up a test task runner and makes it the thread's
// runner. The fixture implements a fake envrionment data collector and a fake
// report uploader.
class IncidentReportingServiceTest : public testing::Test {
 protected:
  // An IRS class that allows a test harness to provide a fake environment
  // collector and report uploader via callbacks.
  class TestIncidentReportingService
      : public safe_browsing::IncidentReportingService {
   public:
    typedef base::Callback<void(Profile*)> PreProfileCreateCallback;

    typedef base::Callback<
        void(safe_browsing::ClientIncidentReport_EnvironmentData*)>
        CollectEnvironmentCallback;

    typedef base::Callback<scoped_ptr<safe_browsing::IncidentReportUploader>(
        const safe_browsing::IncidentReportUploader::OnResultCallback&,
        const safe_browsing::ClientIncidentReport& report)> StartUploadCallback;

    TestIncidentReportingService(
        const scoped_refptr<base::TaskRunner>& task_runner,
        const PreProfileCreateCallback& pre_profile_create_callback,
        const CollectEnvironmentCallback& collect_environment_callback,
        const StartUploadCallback& start_upload_callback)
        : IncidentReportingService(NULL, NULL),
          pre_profile_create_callback_(pre_profile_create_callback),
          collect_environment_callback_(collect_environment_callback),
          start_upload_callback_(start_upload_callback) {
      SetCollectEnvironmentHook(&CollectEnvironmentData, task_runner);
      test_instance_.Get().Set(this);
    }

    virtual ~TestIncidentReportingService() { test_instance_.Get().Set(NULL); }

   protected:
    virtual void OnProfileCreated(Profile* profile) OVERRIDE {
      pre_profile_create_callback_.Run(profile);
      safe_browsing::IncidentReportingService::OnProfileCreated(profile);
    }

    virtual scoped_ptr<safe_browsing::IncidentReportUploader> StartReportUpload(
        const safe_browsing::IncidentReportUploader::OnResultCallback& callback,
        const scoped_refptr<net::URLRequestContextGetter>&
            request_context_getter,
        const safe_browsing::ClientIncidentReport& report) OVERRIDE {
      return start_upload_callback_.Run(callback, report);
    }

   private:
    static TestIncidentReportingService& current() {
      return *test_instance_.Get().Get();
    }

    static void CollectEnvironmentData(
        safe_browsing::ClientIncidentReport_EnvironmentData* data) {
      current().collect_environment_callback_.Run(data);
    };

    static base::LazyInstance<base::ThreadLocalPointer<
        TestIncidentReportingService> >::Leaky test_instance_;

    PreProfileCreateCallback pre_profile_create_callback_;
    CollectEnvironmentCallback collect_environment_callback_;
    StartUploadCallback start_upload_callback_;
  };

  // A type for specifying whether or not a profile created by CreateProfile
  // participates in safe browsing.
  enum SafeBrowsingDisposition {
    SAFE_BROWSING_OPT_OUT,
    SAFE_BROWSING_OPT_IN,
  };

  // A type for specifying the action to be taken by the test fixture during
  // profile initialization (before NOTIFICATION_PROFILE_CREATED is sent).
  enum OnProfileCreationAction {
    ON_PROFILE_CREATION_NO_ACTION,
    ON_PROFILE_CREATION_ADD_INCIDENT,  // Add an incident to the service.
  };

  static const int64 kIncidentTimeMsec;
  static const char kFakeOsName[];

  IncidentReportingServiceTest()
      : task_runner_(new base::TestSimpleTaskRunner),
        thread_task_runner_handle_(task_runner_),
        profile_manager_(TestingBrowserProcess::GetGlobal()),
        instance_(new TestIncidentReportingService(
            task_runner_,
            base::Bind(&IncidentReportingServiceTest::PreProfileCreate,
                       base::Unretained(this)),
            base::Bind(&IncidentReportingServiceTest::CollectEnvironmentData,
                       base::Unretained(this)),
            base::Bind(&IncidentReportingServiceTest::StartUpload,
                       base::Unretained(this)))),
        upload_result_(safe_browsing::IncidentReportUploader::UPLOAD_SUCCESS),
        environment_collected_(),
        uploader_destroyed_() {}

  virtual void SetUp() OVERRIDE {
    testing::Test::SetUp();
    ASSERT_TRUE(profile_manager_.SetUp());
  }

  // Creates and returns a profile (owned by the profile manager) with or
  // without safe browsing enabled. An incident will be created within
  // PreProfileCreate if requested.
  TestingProfile* CreateProfile(const std::string& profile_name,
                                SafeBrowsingDisposition safe_browsing_opt_in,
                                OnProfileCreationAction on_creation_action) {
    // Create prefs for the profile with safe browsing enabled or not.
    scoped_ptr<TestingPrefServiceSyncable> prefs(
        new TestingPrefServiceSyncable);
    chrome::RegisterUserProfilePrefs(prefs->registry());
    prefs->SetBoolean(prefs::kSafeBrowsingEnabled,
                      safe_browsing_opt_in == SAFE_BROWSING_OPT_IN);

    // Remember whether or not to create an incident.
    profile_properties_[profile_name].on_creation_action = on_creation_action;

    // Boom (or fizzle).
    return profile_manager_.CreateTestingProfile(
        profile_name,
        prefs.PassAs<PrefServiceSyncable>(),
        base::ASCIIToUTF16(profile_name),
        0,              // avatar_id (unused)
        std::string(),  // supervised_user_id (unused)
        TestingProfile::TestingFactories());
  }

  // Configures a callback to run when the next upload is started that will post
  // a task to delete the profile. This task will run before the upload
  // finishes.
  void DeleteProfileOnUpload(Profile* profile) {
    ASSERT_TRUE(on_start_upload_callback_.is_null());
    on_start_upload_callback_ =
        base::Bind(&IncidentReportingServiceTest::DelayedDeleteProfile,
                   base::Unretained(this),
                   profile);
  }

  // Returns an incident suitable for testing.
  scoped_ptr<safe_browsing::ClientIncidentReport_IncidentData>
  MakeTestIncident() {
    scoped_ptr<safe_browsing::ClientIncidentReport_IncidentData> incident(
        new safe_browsing::ClientIncidentReport_IncidentData());
    incident->set_incident_time_msec(kIncidentTimeMsec);
    incident->mutable_tracked_preference();
    return incident.Pass();
  }

  // Adds a test incident to the service.
  void AddTestIncident(Profile* profile) {
    instance_->GetAddIncidentCallback(profile).Run(MakeTestIncident().Pass());
  }

  // Confirms that the test incident was uploaded by the service, then clears
  // the instance for subsequent incidents.
  void ExpectTestIncidentUploaded() {
    ASSERT_TRUE(uploaded_report_);
    ASSERT_EQ(1, uploaded_report_->incident_size());
    ASSERT_TRUE(uploaded_report_->incident(0).has_incident_time_msec());
    ASSERT_EQ(kIncidentTimeMsec,
              uploaded_report_->incident(0).incident_time_msec());
    ASSERT_TRUE(uploaded_report_->has_environment());
    ASSERT_TRUE(uploaded_report_->environment().has_os());
    ASSERT_TRUE(uploaded_report_->environment().os().has_os_name());
    ASSERT_EQ(std::string(kFakeOsName),
              uploaded_report_->environment().os().os_name());

    uploaded_report_.reset();
  }

  void ExpectNoUpload() { ASSERT_FALSE(uploaded_report_); }

  bool HasCollectedEnvironmentData() const { return environment_collected_; }
  bool UploaderDestroyed() const { return uploader_destroyed_; }

  scoped_refptr<base::TestSimpleTaskRunner> task_runner_;
  base::ThreadTaskRunnerHandle thread_task_runner_handle_;
  TestingProfileManager profile_manager_;
  scoped_ptr<safe_browsing::IncidentReportingService> instance_;
  base::Closure on_start_upload_callback_;
  safe_browsing::IncidentReportUploader::Result upload_result_;
  bool environment_collected_;
  scoped_ptr<safe_browsing::ClientIncidentReport> uploaded_report_;
  bool uploader_destroyed_;

 private:
  // A fake IncidentReportUploader that posts a task to provide a given response
  // back to the incident reporting service. It also reports back to the test
  // harness via a closure when it is deleted by the incident reporting service.
  class FakeUploader : public safe_browsing::IncidentReportUploader {
   public:
    FakeUploader(
        const base::Closure& on_deleted,
        const safe_browsing::IncidentReportUploader::OnResultCallback& callback,
        safe_browsing::IncidentReportUploader::Result result)
        : safe_browsing::IncidentReportUploader(callback),
          on_deleted_(on_deleted),
          result_(result) {
      // Post a task that will provide the response.
      base::ThreadTaskRunnerHandle::Get()->PostTask(
          FROM_HERE,
          base::Bind(&FakeUploader::FinishUpload, base::Unretained(this)));
    }
    virtual ~FakeUploader() { on_deleted_.Run(); }

   private:
    void FinishUpload() {
      // Callbacks have a tendency to delete the uploader, so no touching
      // anything after this.
      callback_.Run(result_,
                    scoped_ptr<safe_browsing::ClientIncidentResponse>());
    }

    base::Closure on_deleted_;
    safe_browsing::IncidentReportUploader::Result result_;

    DISALLOW_COPY_AND_ASSIGN(FakeUploader);
  };

  // Properties for a profile that impact the behavior of the test.
  struct ProfileProperties {
    ProfileProperties() : on_creation_action(ON_PROFILE_CREATION_NO_ACTION) {}

    // The action taken by the test fixture during profile initialization
    // (before NOTIFICATION_PROFILE_CREATED is sent).
    OnProfileCreationAction on_creation_action;
  };

  // Returns the name of a profile as provided to CreateProfile.
  static std::string GetProfileName(Profile* profile) {
    // Cannot reliably use profile->GetProfileName() since the test needs the
    // name before the profile manager sets it (which happens after profile
    // creation).
    return profile->GetPath().BaseName().AsUTF8Unsafe();
  }

  // Posts a task to delete the profile.
  void DelayedDeleteProfile(Profile* profile) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::Bind(&TestingProfileManager::DeleteTestingProfile,
                   base::Unretained(&profile_manager_),
                   GetProfileName(profile)));
  }

  // A callback run by the test fixture when a profile is created. An incident
  // is added.
  void PreProfileCreate(Profile* profile) {
    // The instance must have already been created.
    ASSERT_TRUE(instance_);
    // Add a test incident to the service if requested.
    switch (profile_properties_[GetProfileName(profile)].on_creation_action) {
      case ON_PROFILE_CREATION_ADD_INCIDENT:
        AddTestIncident(profile);
        break;
      default:
        ASSERT_EQ(
            ON_PROFILE_CREATION_NO_ACTION,
            profile_properties_[GetProfileName(profile)].on_creation_action);
        break;
    }
  }

  // A fake CollectEnvironmentData implementation invoked by the service during
  // operation.
  void CollectEnvironmentData(
      safe_browsing::ClientIncidentReport_EnvironmentData* data) {
    ASSERT_NE(
        static_cast<safe_browsing::ClientIncidentReport_EnvironmentData*>(NULL),
        data);
    data->mutable_os()->set_os_name(kFakeOsName);
    environment_collected_ = true;
  }

  // A fake StartUpload implementation invoked by the service during operation.
  scoped_ptr<safe_browsing::IncidentReportUploader> StartUpload(
      const safe_browsing::IncidentReportUploader::OnResultCallback& callback,
      const safe_browsing::ClientIncidentReport& report) {
    // Remember the report that is being uploaded.
    uploaded_report_.reset(new safe_browsing::ClientIncidentReport(report));
    // Run and clear the OnStartUpload callback, if provided.
    if (!on_start_upload_callback_.is_null()) {
      on_start_upload_callback_.Run();
      on_start_upload_callback_ = base::Closure();
    }
    return scoped_ptr<safe_browsing::IncidentReportUploader>(new FakeUploader(
        base::Bind(&IncidentReportingServiceTest::OnUploaderDestroyed,
                   base::Unretained(this)),
        callback,
        upload_result_));
  }

  void OnUploaderDestroyed() { uploader_destroyed_ = true; }

  // A mapping of profile name to its corresponding properties.
  std::map<std::string, ProfileProperties> profile_properties_;
};

// static
base::LazyInstance<base::ThreadLocalPointer<
    IncidentReportingServiceTest::TestIncidentReportingService> >::Leaky
    IncidentReportingServiceTest::TestIncidentReportingService::test_instance_ =
        LAZY_INSTANCE_INITIALIZER;

const int64 IncidentReportingServiceTest::kIncidentTimeMsec = 47LL;
const char IncidentReportingServiceTest::kFakeOsName[] = "fakedows";

// Tests that an incident added during profile initialization when safe browsing
// is on is uploaded.
TEST_F(IncidentReportingServiceTest, AddIncident) {
  CreateProfile(
      "profile1", SAFE_BROWSING_OPT_IN, ON_PROFILE_CREATION_ADD_INCIDENT);

  // Let all tasks run.
  task_runner_->RunUntilIdle();

  // Verify that environment collection took place.
  EXPECT_TRUE(HasCollectedEnvironmentData());

  // Verify that report upload took place and contained the incident and
  // environment data.
  ExpectTestIncidentUploaded();

  // Verify that the uploader was destroyed.
  ASSERT_TRUE(UploaderDestroyed());
}

// Tests that an incident added during profile initialization when safe browsing
// is off is not uploaded.
TEST_F(IncidentReportingServiceTest, NoSafeBrowsing) {
  // Create the profile, thereby causing the test to begin.
  CreateProfile(
      "profile1", SAFE_BROWSING_OPT_OUT, ON_PROFILE_CREATION_ADD_INCIDENT);

  // Let all tasks run.
  task_runner_->RunUntilIdle();

  // Verify that no report upload took place.
  ExpectNoUpload();
}

// Tests that an incident added after upload is not uploaded again.
TEST_F(IncidentReportingServiceTest, OnlyOneUpload) {
  // Create the profile, thereby causing the test to begin.
  Profile* profile = CreateProfile(
      "profile1", SAFE_BROWSING_OPT_IN, ON_PROFILE_CREATION_ADD_INCIDENT);

  // Let all tasks run.
  task_runner_->RunUntilIdle();

  // Verify that report upload took place and contained the incident and
  // environment data.
  ExpectTestIncidentUploaded();

  // Add the incident to the service again.
  AddTestIncident(profile);

  // Let all tasks run.
  task_runner_->RunUntilIdle();

  // Verify that no additional report upload took place.
  ExpectNoUpload();
}

// Tests that the same incident added for two different profiles in sequence
// results in two uploads.
TEST_F(IncidentReportingServiceTest, TwoProfilesTwoUploads) {
  // Create the profile, thereby causing the test to begin.
  CreateProfile(
      "profile1", SAFE_BROWSING_OPT_IN, ON_PROFILE_CREATION_ADD_INCIDENT);

  // Let all tasks run.
  task_runner_->RunUntilIdle();

  // Verify that report upload took place and contained the incident and
  // environment data.
  ExpectTestIncidentUploaded();

  // Create a second profile with its own incident on creation.
  CreateProfile(
      "profile2", SAFE_BROWSING_OPT_IN, ON_PROFILE_CREATION_ADD_INCIDENT);

  // Let all tasks run.
  task_runner_->RunUntilIdle();

  // Verify that a second report upload took place.
  ExpectTestIncidentUploaded();
}

// Tests that an upload succeeds if the profile is destroyed while it is
// pending.
TEST_F(IncidentReportingServiceTest, ProfileDestroyedDuringUpload) {
  // Create a profile for which an incident will be added.
  Profile* profile = CreateProfile(
      "profile1", SAFE_BROWSING_OPT_IN, ON_PROFILE_CREATION_ADD_INCIDENT);

  // Hook up a callback to run when the upload is started that will post a task
  // to delete the profile. This task will run before the upload finishes.
  DeleteProfileOnUpload(profile);

  // Let all tasks run.
  task_runner_->RunUntilIdle();

  // Verify that report upload took place and contained the incident and
  // environment data.
  ExpectTestIncidentUploaded();

  // The lack of a crash indicates that the deleted profile was not accessed by
  // the service while handling the upload response.
}

// Parallel uploads
// Shutdown during processing
// environment colection taking longer than incident delay timer
// environment colection taking longer than incident delay timer, and then
// another incident arriving
