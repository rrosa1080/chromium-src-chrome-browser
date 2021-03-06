// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_DEVTOOLS_CHROME_DEVTOOLS_MANAGER_DELEGATE_H_
#define CHROME_BROWSER_DEVTOOLS_CHROME_DEVTOOLS_MANAGER_DELEGATE_H_

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "chrome/browser/devtools/devtools_protocol.h"
#include "content/public/browser/devtools_manager_delegate.h"

class DevToolsNetworkConditions;
class Profile;

class ChromeDevToolsManagerDelegate : public content::DevToolsManagerDelegate {
 public:
  ChromeDevToolsManagerDelegate();
  virtual ~ChromeDevToolsManagerDelegate();

  // content::DevToolsManagerDelegate overrides:
  virtual void Inspect(content::BrowserContext* browser_context,
                       content::DevToolsAgentHost* agent_host) OVERRIDE;
  virtual base::DictionaryValue* HandleCommand(
      content::DevToolsAgentHost* agent_host,
      base::DictionaryValue* command_dict) OVERRIDE;

 private:
  base::Callback<void(content::DevToolsAgentHost*, bool)> devtools_callback_;
  bool devtools_callback_registered_;
  void EnsureDevtoolsCallbackRegistered();

  Profile* GetProfile(content::DevToolsAgentHost* agent_host);

  scoped_ptr<DevToolsProtocol::Response> EmulateNetworkConditions(
      content::DevToolsAgentHost* agent_host,
      DevToolsProtocol::Command* command);

  void UpdateNetworkState(
      content::DevToolsAgentHost* agent_host,
      const scoped_refptr<DevToolsNetworkConditions> conditions);

  void OnDevToolsStateChanged(content::DevToolsAgentHost* agent_host,
                              bool attached);

  DISALLOW_COPY_AND_ASSIGN(ChromeDevToolsManagerDelegate);
};

#endif  // CHROME_BROWSER_DEVTOOLS_CHROME_DEVTOOLS_MANAGER_DELEGATE_H_
