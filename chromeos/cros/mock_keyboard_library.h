// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_CROS_MOCK_KEYBOARD_LIBRARY_H_
#define CHROME_BROWSER_CHROMEOS_CROS_MOCK_KEYBOARD_LIBRARY_H_
#pragma once

#include <string>

#include "chrome/browser/chromeos/cros/keyboard_library.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace chromeos {

class MockKeyboardLibrary : public KeyboardLibrary {
 public:
  MockKeyboardLibrary() {}
  virtual ~MockKeyboardLibrary() {}

  MOCK_CONST_METHOD0(GetCurrentKeyboardLayoutName, std::string(void));
  MOCK_METHOD1(SetCurrentKeyboardLayoutByName, bool(const std::string&));
  MOCK_CONST_METHOD1(GetKeyboardLayoutPerWindow, bool(bool*));
  MOCK_METHOD1(SetKeyboardLayoutPerWindow, bool(bool));
};

}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_CROS_MOCK_KEYBOARD_LIBRARY_H_
