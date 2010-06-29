// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/dom_ui/core_options_handler.h"

#include "app/l10n_util.h"
#include "app/resource_bundle.h"
#include "base/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/common/notification_service.h"
#include "chrome/common/notification_type.h"
#include "chrome/browser/pref_service.h"
#include "chrome/browser/profile.h"
#include "grit/browser_resources.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "grit/locale_settings.h"
#include "grit/theme_resources.h"

CoreOptionsHandler::CoreOptionsHandler() {
}

void CoreOptionsHandler::GetLocalizedValues(
    DictionaryValue* localized_strings) {
  DCHECK(localized_strings);
  // Main
  localized_strings->SetString(L"title",
      l10n_util::GetStringF(IDS_OPTIONS_DIALOG_TITLE,
          l10n_util::GetString(IDS_PRODUCT_NAME)));

#if defined(OS_CHROMEOS)
  localized_strings->SetString(L"systemPage",
      l10n_util::GetString(IDS_OPTIONS_SYSTEM_TAB_LABEL));
  localized_strings->SetString(L"internetPage",
      l10n_util::GetString(IDS_OPTIONS_INTERNET_TAB_LABEL));
#endif

  localized_strings->SetString(L"basicsPage",
      l10n_util::GetString(IDS_OPTIONS_GENERAL_TAB_LABEL));
  localized_strings->SetString(L"personalStuffPage",
      l10n_util::GetString(IDS_OPTIONS_CONTENT_TAB_LABEL));
  localized_strings->SetString(L"underHoodPage",
      l10n_util::GetString(IDS_OPTIONS_ADVANCED_TAB_LABEL));
}

void CoreOptionsHandler::Observe(NotificationType type,
                                 const NotificationSource& source,
                                 const NotificationDetails& details) {
  if (type == NotificationType::PREF_CHANGED)
    NotifyPrefChanged(Details<std::wstring>(details).ptr());
}

void CoreOptionsHandler::RegisterMessages() {
  dom_ui_->RegisterMessageCallback("fetchPrefs",
      NewCallback(this, &CoreOptionsHandler::HandleFetchPrefs));
  dom_ui_->RegisterMessageCallback("observePrefs",
      NewCallback(this, &CoreOptionsHandler::HandleFetchPrefs));
  dom_ui_->RegisterMessageCallback("setBooleanPref",
      NewCallback(this, &CoreOptionsHandler::HandleSetBooleanPref));
  dom_ui_->RegisterMessageCallback("setIntegerPref",
      NewCallback(this, &CoreOptionsHandler::HandleSetIntegerPref));
  dom_ui_->RegisterMessageCallback("setStringPref",
      NewCallback(this, &CoreOptionsHandler::HandleSetStringPref));
}


void CoreOptionsHandler::HandleFetchPrefs(const Value* value) {
  if (!value || !value->IsType(Value::TYPE_LIST))
    return;

  const ListValue* param_values = static_cast<const ListValue*>(value);

  // First param is name of callback function, the second one is the value of
  // context that is just passed through - so, there needs to be at least one
  // more for the actual preference identifier.
  const size_t kMinFetchPrefsParamCount = 3;
  if (param_values->GetSize() < kMinFetchPrefsParamCount)
    return;

  size_t idx = param_values->GetSize();
  LOG(INFO) << "param_values->GetSize() = " << idx;
  // Get callback JS function name.
  Value* callback;
  if (!param_values->Get(0, &callback) || !callback->IsType(Value::TYPE_STRING))
    return;

  std::wstring callback_function;
  if (!callback->GetAsString(&callback_function))
    return;

  // Get context param (just passthrough value)
  Value* context;
  if (!param_values->Get(1, &context) || !context)
    return;

  // Get the list of name for prefs to build the response dictionary.
  DictionaryValue result_value;
  Value* list_member;
  DCHECK(dom_ui_);
  PrefService* pref_service = dom_ui_->GetProfile()->GetPrefs();

  for (size_t i = 2; i < param_values->GetSize(); i++) {
    if (!param_values->Get(i, &list_member))
      break;

    if (!list_member->IsType(Value::TYPE_STRING))
      continue;

    std::wstring pref_name;
    if (!list_member->GetAsString(&pref_name))
      continue;

    const PrefService::Preference* pref =
        pref_service->FindPreference(pref_name.c_str());
    result_value.Set(pref_name.c_str(),
        pref ? pref->GetValue()->DeepCopy() : Value::CreateNullValue());
  }
  dom_ui_->CallJavascriptFunction(callback_function.c_str(), *context,
                                  result_value);
}

void CoreOptionsHandler::HandleObservePefs(const Value* value) {
  if (!value || !value->IsType(Value::TYPE_LIST))
    return;

  DCHECK(dom_ui_);
  PrefService* pref_service = dom_ui_->GetProfile()->GetPrefs();
  DictionaryValue result_value;
  const ListValue* list_value = static_cast<const ListValue*>(value);
  Value* list_member;
  for (size_t i = 0; i < list_value->GetSize(); i++) {
    if (!list_value->Get(i, &list_member))
      break;

    if (!list_member->IsType(Value::TYPE_STRING))
      continue;

    std::wstring pref_name;
    if (!list_member->GetAsString(&pref_name))
      continue;

    pref_service->AddPrefObserver(pref_name.c_str(), this);
  }
}

void CoreOptionsHandler::HandleSetBooleanPref(const Value* value) {
  HandleSetPref(value, Value::TYPE_BOOLEAN);
}

void CoreOptionsHandler::HandleSetIntegerPref(const Value* value) {
  HandleSetPref(value, Value::TYPE_INTEGER);
}

void CoreOptionsHandler::HandleSetStringPref(const Value* value) {
  HandleSetPref(value, Value::TYPE_STRING);
}

void CoreOptionsHandler::HandleSetPref(const Value* value,
                                       Value::ValueType type) {
  if (!value || !value->IsType(Value::TYPE_LIST))
    return;
  const ListValue* param_values = static_cast<const ListValue*>(value);
  size_t size = param_values->GetSize();
  LOG(INFO) << "Array size = " << size;
  if (param_values->GetSize() != 2)
    return;

  DCHECK(dom_ui_);
  PrefService* pref_service = dom_ui_->GetProfile()->GetPrefs();

  Value* name_element;
  std::wstring pref_name;
  if (!param_values->Get(0, &name_element) ||
      !name_element->IsType(Value::TYPE_STRING) ||
      !name_element->GetAsString(&pref_name))
    return;

  Value* value_element;
  std::string value_string;
  if (!param_values->Get(1, &value_element) ||
      !value_element->IsType(Value::TYPE_STRING) ||
      !value_element->GetAsString(&value_string))
    return;

  switch (type) {
    case Value::TYPE_BOOLEAN:
      pref_service->SetBoolean(pref_name.c_str(), value_string == "true");
      break;
    case Value::TYPE_INTEGER:
      int int_value;
      if (StringToInt(value_string, &int_value))
        pref_service->SetInteger(pref_name.c_str(), int_value);
      break;
    case Value::TYPE_STRING:
      pref_service->SetString(pref_name.c_str(), value_string);
      break;
    default:
      NOTREACHED();
  }
}

void CoreOptionsHandler::NotifyPrefChanged(const std::wstring* pref_name) {
  DCHECK(dom_ui_);
  PrefService* pref_service = dom_ui_->GetProfile()->GetPrefs();
  const PrefService::Preference* pref =
      pref_service->FindPreference(pref_name->c_str());
  if (pref) {
    DictionaryValue result_value;
    result_value.Set(pref_name->c_str(), pref->GetValue()->DeepCopy());
    dom_ui_->CallJavascriptFunction(L"prefsChanged", result_value);
  }
}
