// Copyright 2025 LiveKit, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

// This must be included before many other Windows headers.
#include <windows.h>

#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <vector>
#include <mutex>

// A small manager that opts-out this process's audio sessions from the
// Windows default communications ducking experience.
//
// It iterates existing sessions on all active render endpoints and sets
// IAudioSessionControl2::SetDuckingPreference(TRUE) for sessions that belong
// to the current process. It also registers IAudioSessionNotification to catch
// new sessions created later and applies the same preference.
class AudioDuckingManager {
 public:
  AudioDuckingManager();
  ~AudioDuckingManager();

  // Initializes COM (if needed), enumerates endpoints, sets ducking
  // preference for existing sessions and registers notifications for new ones.
  // Safe to call multiple times; subsequent calls are no-ops.
  void Initialize();

 private:
  // Registration record for a single endpoint's session manager.
  struct Registration {
    IAudioSessionManager2* manager2 = nullptr;  // owned; AddRef'ed
    IAudioSessionNotification* notifier = nullptr;  // owned; AddRef'ed
  };

  // Apply ducking preference TRUE to all existing sessions of this process
  // for a given session manager.
  void ApplyOptOutToExistingSessions(IAudioSessionManager2* manager2);

  // Registers a notification object with the given manager to handle future
  // sessions.
  void RegisterForNewSessions(IAudioSessionManager2* manager2);

  // Helper to create a session manager for the given device id string.
  IAudioSessionManager2* CreateSessionManagerForDevice(IMMDevice* device);

  // Enumerate all active render endpoints and perform setup.
  void SetupForAllRenderDevices();

  // COM initialization tracking for this instance's thread.
  bool com_initialized_ = false;

  std::vector<Registration> registrations_;
  std::mutex mutex_;
};
