// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/supervised_user/chromeos/manager_password_service_factory.h"

#include "chrome/browser/chromeos/login/users/supervised_user_manager.h"
#include "chrome/browser/chromeos/login/users/user.h"
#include "chrome/browser/chromeos/login/users/user_manager.h"
#include "chrome/browser/profiles/incognito_helpers.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/supervised_user/chromeos/manager_password_service.h"
#include "chrome/browser/supervised_user/supervised_user_shared_settings_service_factory.h"
#include "chrome/browser/supervised_user/supervised_user_sync_service_factory.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"

namespace chromeos {

// static
ManagerPasswordService*
ManagerPasswordServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<ManagerPasswordService*>(
      GetInstance()->GetServiceForBrowserContext(profile, true));
}

// static
ManagerPasswordServiceFactory*
ManagerPasswordServiceFactory::GetInstance() {
  return Singleton<ManagerPasswordServiceFactory>::get();
}

ManagerPasswordServiceFactory::ManagerPasswordServiceFactory()
    : BrowserContextKeyedServiceFactory(
          "ManagerPasswordService",
          BrowserContextDependencyManager::GetInstance()) {
  DependsOn(SupervisedUserSharedSettingsServiceFactory::GetInstance());
  DependsOn(SupervisedUserSyncServiceFactory::GetInstance());
}

ManagerPasswordServiceFactory::
    ~ManagerPasswordServiceFactory() {}

KeyedService* ManagerPasswordServiceFactory::BuildServiceInstanceFor(
    content::BrowserContext* context) const {
  Profile* profile= static_cast<Profile*>(context);
  chromeos::User* user = chromeos::UserManager::Get()->
      GetUserByProfile(profile);
  if (chromeos::UserManager::Get()->GetSupervisedUserManager()->
      HasSupervisedUsers(user->email())) {
    ManagerPasswordService* result = new ManagerPasswordService();
    result->Init(
        user->email(),
        SupervisedUserSyncServiceFactory::GetForProfile(profile),
        SupervisedUserSharedSettingsServiceFactory::GetForBrowserContext(
            profile));
    return result;
  }
  return NULL;
}

content::BrowserContext*
ManagerPasswordServiceFactory::GetBrowserContextToUse(
    content::BrowserContext* context) const {
  return chrome::GetBrowserContextRedirectedInIncognito(context);
}

}  // namespace chromeos
