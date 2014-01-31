// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/services/gcm/gcm_client_mock.h"

#include "base/bind.h"
#include "base/logging.h"
#include "base/message_loop/message_loop.h"
#include "base/sys_byteorder.h"
#include "base/time/time.h"
#include "content/public/browser/browser_thread.h"

namespace gcm {

namespace {

// Converts the 8-byte prefix of a string into a uint64 value.
uint64 HashToUInt64(const std::string& hash) {
  uint64 value;
  DCHECK_GE(hash.size(), sizeof(value));
  memcpy(&value, hash.data(), sizeof(value));
  return base::HostToNet64(value);
}

}  // namespace

GCMClientMock::GCMClientMock()
    : ready_(true),
      simulate_server_error_(false) {
}

GCMClientMock::~GCMClientMock() {
}

void GCMClientMock::SetUserDelegate(const std::string& username,
                                    Delegate* delegate) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::IO));

  if (delegate)
    delegates_[username] = delegate;
  else
    delegates_.erase(username);
}

void GCMClientMock::CheckIn(const std::string& username) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::IO));

  // Simulate the android_id and secret by some sort of hashing.
  CheckinInfo checkin_info;
  if (!simulate_server_error_)
    checkin_info = GetCheckinInfoFromUsername(username);

  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&GCMClientMock::CheckInFinished,
                 base::Unretained(this),
                 username,
                 checkin_info));
}

void GCMClientMock::Register(const std::string& username,
                             const std::string& app_id,
                             const std::string& cert,
                             const std::vector<std::string>& sender_ids) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::IO));

  std::string registration_id;
  if (!simulate_server_error_)
    registration_id = GetRegistrationIdFromSenderIds(sender_ids);

  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&GCMClientMock::RegisterFinished,
                 base::Unretained(this),
                 username,
                 app_id,
                 registration_id));
}

void GCMClientMock::Unregister(const std::string& username,
                               const std::string& app_id) {
}

void GCMClientMock::Send(const std::string& username,
                         const std::string& app_id,
                         const std::string& receiver_id,
                         const OutgoingMessage& message) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::IO));

  base::MessageLoop::current()->PostTask(
      FROM_HERE,
      base::Bind(&GCMClientMock::SendFinished,
                 base::Unretained(this),
                 username,
                 app_id,
                 message.id));
}

bool GCMClientMock::IsReady() const {
  return ready_;
}

void GCMClientMock::ReceiveMessage(const std::string& username,
                                   const std::string& app_id,
                                   const IncomingMessage& message) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));

  content::BrowserThread::PostTask(
      content::BrowserThread::IO,
      FROM_HERE,
      base::Bind(&GCMClientMock::MessageReceived,
                 base::Unretained(this),
                 username,
                 app_id,
                 message));
}

void GCMClientMock::DeleteMessages(const std::string& username,
                                   const std::string& app_id) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));

  content::BrowserThread::PostTask(
      content::BrowserThread::IO,
      FROM_HERE,
      base::Bind(&GCMClientMock::MessagesDeleted,
                 base::Unretained(this),
                 username,
                 app_id));
}

void GCMClientMock::SetReady(bool ready) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));

  if (ready == ready_)
    return;
  ready_ = ready;

  if (!ready_)
    return;
  content::BrowserThread::PostTask(
      content::BrowserThread::IO,
      FROM_HERE,
      base::Bind(&GCMClientMock::SetReadyOnIO,
                 base::Unretained(this)));
}

// static
GCMClient::CheckinInfo GCMClientMock::GetCheckinInfoFromUsername(
    const std::string& username) {
  CheckinInfo checkin_info;
  checkin_info.android_id = HashToUInt64(username);
  checkin_info.secret = checkin_info.android_id / 10;
  return checkin_info;
}

// static
std::string GCMClientMock::GetRegistrationIdFromSenderIds(
    const std::vector<std::string>& sender_ids) {
  // GCMProfileService normalizes the sender IDs by making them sorted.
  std::vector<std::string> normalized_sender_ids = sender_ids;
  std::sort(normalized_sender_ids.begin(), normalized_sender_ids.end());

  // Simulate the registration_id by concaternating all sender IDs.
  // Set registration_id to empty to denote an error if sender_ids contains a
  // hint.
  std::string registration_id;
  if (sender_ids.size() != 1 ||
      sender_ids[0].find("error") == std::string::npos) {
    for (size_t i = 0; i < normalized_sender_ids.size(); ++i) {
      if (i > 0)
        registration_id += ",";
      registration_id += normalized_sender_ids[i];
    }
  }
  return registration_id;
}

GCMClient::Delegate* GCMClientMock::GetDelegate(
    const std::string& username) const {
  std::map<std::string, Delegate*>::const_iterator iter =
      delegates_.find(username);
  return iter == delegates_.end() ? NULL : iter->second;
}

void GCMClientMock::CheckInFinished(std::string username,
                                    CheckinInfo checkin_info) {
  GCMClient::Delegate* delegate = GetDelegate(username);
  DCHECK(delegate);
  delegate->OnCheckInFinished(
      checkin_info, checkin_info.IsValid() ? SUCCESS : SERVER_ERROR);
}

void GCMClientMock::RegisterFinished(std::string username,
                                     std::string app_id,
                                     std::string registrion_id) {
  GCMClient::Delegate* delegate = GetDelegate(username);
  DCHECK(delegate);
  delegate->OnRegisterFinished(
      app_id, registrion_id, registrion_id.empty() ? SERVER_ERROR : SUCCESS);
}

void GCMClientMock::SendFinished(std::string username,
                                 std::string app_id,
                                 std::string message_id) {
  GCMClient::Delegate* delegate = GetDelegate(username);
  DCHECK(delegate);
  delegate->OnSendFinished(app_id, message_id, SUCCESS);

  // Simulate send error if message id contains a hint.
  if (message_id.find("error") != std::string::npos) {
    base::MessageLoop::current()->PostDelayedTask(
        FROM_HERE,
        base::Bind(&GCMClientMock::MessageSendError,
                   base::Unretained(this),
                   username,
                   app_id,
                   message_id),
        base::TimeDelta::FromMilliseconds(200));
  }
}

void GCMClientMock::MessageReceived(std::string username,
                                    std::string app_id,
                                    IncomingMessage message) {
  GCMClient::Delegate* delegate = GetDelegate(username);
  if (delegate)
    delegate->OnMessageReceived(app_id, message);
}

void GCMClientMock::MessagesDeleted(std::string username, std::string app_id) {
  GCMClient::Delegate* delegate = GetDelegate(username);
  if (delegate)
    delegate->OnMessagesDeleted(app_id);
}

void GCMClientMock::MessageSendError(std::string username,
                                     std::string app_id,
                                     std::string message_id) {
  GCMClient::Delegate* delegate = GetDelegate(username);
  if (delegate)
    delegate->OnMessageSendError(app_id, message_id, NETWORK_ERROR);
}

void GCMClientMock::SetReadyOnIO() {
  for (std::map<std::string, Delegate*>::const_iterator iter =
           delegates_.begin();
       iter != delegates_.end(); ++iter) {
    iter->second->OnGCMReady();
  }
}

}  // namespace gcm
