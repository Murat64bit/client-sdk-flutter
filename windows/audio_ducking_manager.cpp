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

#include "audio_ducking_manager.h"

#include <functiondiscoverykeys_devpkey.h>

#include <atomic>

namespace {

template <typename T>
void SafeRelease(T** pp) {
  if (pp && *pp) {
    (*pp)->Release();
    *pp = nullptr;
  }
}

class SessionNotifier : public IAudioSessionNotification {
 public:
  explicit SessionNotifier() : ref_count_(1) {}

  // IUnknown
  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification)) {
      *ppv = static_cast<IAudioSessionNotification*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  IFACEMETHODIMP_(ULONG) AddRef() override {
    return ++ref_count_;
  }

  IFACEMETHODIMP_(ULONG) Release() override {
    ULONG r = --ref_count_;
    if (r == 0) {
      delete this;
    }
    return r;
  }

  // IAudioSessionNotification
  IFACEMETHODIMP OnSessionCreated(IAudioSessionControl* new_session) override {
    if (!new_session) return S_OK;

    IAudioSessionControl2* control2 = nullptr;
    if (SUCCEEDED(new_session->QueryInterface(__uuidof(IAudioSessionControl2),
                                              reinterpret_cast<void**>(&control2)))) {
      DWORD pid = 0;
      if (SUCCEEDED(control2->GetProcessId(&pid))) {
        if (pid == ::GetCurrentProcessId()) {
          // Opt-out of Windows default ducking for this session.
          control2->SetDuckingPreference(TRUE);
        }
      }
    }
    SafeRelease(&control2);
    return S_OK;
  }

 private:
  std::atomic<ULONG> ref_count_;
};

}  // namespace

AudioDuckingManager::AudioDuckingManager() = default;
AudioDuckingManager::~AudioDuckingManager() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& r : registrations_) {
    if (r.manager2 && r.notifier) {
      r.manager2->UnregisterSessionNotification(r.notifier);
    }
    SafeRelease(&r.notifier);
    SafeRelease(&r.manager2);
  }
  registrations_.clear();
  if (com_initialized_) {
    ::CoUninitialize();
    com_initialized_ = false;
  }
}

void AudioDuckingManager::Initialize() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!com_initialized_) {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
      com_initialized_ = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
      // COM already initialized with different model; proceed without marking.
      // Calls below still work on this thread.
    }
  }

  SetupForAllRenderDevices();
}

void AudioDuckingManager::SetupForAllRenderDevices() {
  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDeviceCollection* collection = nullptr;

  HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || !enumerator) {
    SafeRelease(&enumerator);
    return;
  }

  hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
  if (FAILED(hr) || !collection) {
    SafeRelease(&collection);
    SafeRelease(&enumerator);
    return;
  }

  UINT count = 0;
  collection->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* device = nullptr;
    if (SUCCEEDED(collection->Item(i, &device)) && device) {
      IAudioSessionManager2* manager2 = CreateSessionManagerForDevice(device);
      if (manager2) {
        ApplyOptOutToExistingSessions(manager2);
        RegisterForNewSessions(manager2);
      }
    }
    SafeRelease(&device);
  }

  SafeRelease(&collection);
  SafeRelease(&enumerator);
}

IAudioSessionManager2* AudioDuckingManager::CreateSessionManagerForDevice(
    IMMDevice* device) {
  if (!device) return nullptr;
  IAudioSessionManager2* manager2 = nullptr;
  HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                nullptr, reinterpret_cast<void**>(&manager2));
  if (FAILED(hr)) {
    SafeRelease(&manager2);
    return nullptr;
  }
  return manager2;
}

void AudioDuckingManager::ApplyOptOutToExistingSessions(
    IAudioSessionManager2* manager2) {
  if (!manager2) return;

  IAudioSessionEnumerator* enumerator = nullptr;
  if (FAILED(manager2->GetSessionEnumerator(&enumerator)) || !enumerator) {
    SafeRelease(&enumerator);
    return;
  }

  int count = 0;
  if (FAILED(enumerator->GetCount(&count))) {
    SafeRelease(&enumerator);
    return;
  }

  DWORD my_pid = ::GetCurrentProcessId();

  for (int i = 0; i < count; ++i) {
    IAudioSessionControl* control = nullptr;
    if (SUCCEEDED(enumerator->GetSession(i, &control)) && control) {
      IAudioSessionControl2* control2 = nullptr;
      if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                            reinterpret_cast<void**>(&control2))) &&
          control2) {
        DWORD pid = 0;
        if (SUCCEEDED(control2->GetProcessId(&pid)) && pid == my_pid) {
          control2->SetDuckingPreference(TRUE);
        }
      }
      SafeRelease(&control2);
    }
    SafeRelease(&control);
  }

  SafeRelease(&enumerator);
}

void AudioDuckingManager::RegisterForNewSessions(IAudioSessionManager2* manager2) {
  if (!manager2) return;

  // Create a notifier and register it.
  SessionNotifier* notifier = new SessionNotifier();
  if (SUCCEEDED(manager2->RegisterSessionNotification(notifier))) {
    Registration reg{};
    manager2->AddRef();
    reg.manager2 = manager2;
    reg.notifier = notifier;  // ownership kept; will Release on destruction
    registrations_.push_back(reg);
  } else {
    notifier->Release();
  }
}
