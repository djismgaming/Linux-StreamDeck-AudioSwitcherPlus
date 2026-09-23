/* Copyright (c) 2026 Morgan Scruggs
 *
 * This source code is licensed under the MIT-style license found in the
 * LICENSE file.
 *
 * Linux (PulseAudio / PipeWire) backend for the FredEmmott::Audio API used by
 * the Audio Switcher Plus Stream Deck plugin.
 *
 * The upstream AudioDeviceLib only ships Windows and macOS backends; this
 * file implements the same small interface on top of libpulse, which talks to
 * PipeWire's PulseAudio-compatible server on modern Linux desktops.
 *
 * Device identity maps PulseAudio's per-port model onto the plugin's device
 * model: every selectable port of a sink/source (e.g. "Line Out" vs.
 * "Headphones" on the same analog card) is exposed as its own device, so
 * buttons can switch between ports of one card:
 *
 *   device with ports:  id            = "<name>|<port name>"
 *                       interfaceName = "<description> - <port description>"
 *                       endpointName  = "<port description>"
 *   device w/o ports:   id            = "<name>"
 *                       interfaceName = "<description>"
 *
 * PulseAudio only has one default device per direction, so both roles
 * (DEFAULT and COMMUNICATION) map onto it.
 */

#include <cstdint>

#include <AudioDevices/AudioDevices.h>

#include <pulse/pulseaudio.h>
#include <pulse/rtclock.h>
#include <pulse/thread-mainloop.h>
#include <pulse/timeval.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace FredEmmott::Audio {

struct DefaultChangeCallbackHandle::Impl {
  std::function<void(AudioDeviceDirection, AudioDeviceRole, const std::string&)>
    callback;
};

// The header declares these handle constructors/destructors out-of-line;
// upstream's platform backends define them in their own .cpp files.
DefaultChangeCallbackHandle::DefaultChangeCallbackHandle(
  const std::shared_ptr<Impl>& p)
  : p(p) {
}
DefaultChangeCallbackHandle::~DefaultChangeCallbackHandle() = default;
MuteCallbackHandle::MuteCallbackHandle(const std::shared_ptr<Impl>& p) : p(p) {
}
MuteCallbackHandle::~MuteCallbackHandle() = default;
VolumeCallbackHandle::VolumeCallbackHandle(const std::shared_ptr<Impl>& p)
  : p(p) {
}
VolumeCallbackHandle::~VolumeCallbackHandle() = default;

namespace {

void Log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::fputs("[audioswitcherplus] ", stderr);
  std::vfprintf(stderr, fmt, args);
  std::fputc('\n', stderr);
  va_end(args);
}

/** "<sink>|<port>" -> {"<sink>", "<port>"}; "<sink>" -> {"<sink>", ""} */
std::pair<std::string, std::string> SplitDeviceID(const std::string& id) {
  const auto separator = id.find('|');
  if (separator == std::string::npos) {
    return {id, {}};
  }
  return {id.substr(0, separator), id.substr(separator + 1)};
}

struct PortInfo {
  std::string name;
  std::string description;
  int available;
};

/** Scratch space for one in-flight device enumeration. */
struct RefreshData {
  class Backend* backend { nullptr };
  std::mutex mutex;
  int pending { 3 };// server info + sink list + source list
  std::string defaultSinkName;
  std::string defaultSourceName;
  std::map<std::string, AudioDeviceInfo> outputs;
  std::map<std::string, AudioDeviceInfo> inputs;
  // device name -> active port name ("" when the device has no ports)
  std::map<std::string, std::string> sinkActivePort;
  std::map<std::string, std::string> sourceActivePort;
};

class Backend {
 public:
  static Backend& Get() {
    static Backend instance;
    return instance;
  }

  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  std::map<std::string, AudioDeviceInfo> GetDeviceList(
    AudioDeviceDirection direction) {
    EnsureStarted();
    std::scoped_lock lock(mStateMutex);
    return direction == AudioDeviceDirection::OUTPUT ? mOutputs : mInputs;
  }

  AudioDeviceState GetDeviceState(const std::string& id) {
    EnsureStarted();
    std::scoped_lock lock(mStateMutex);
    const auto output = mOutputs.find(id);
    if (output != mOutputs.end()) {
      return output->second.state;
    }
    const auto input = mInputs.find(id);
    if (input != mInputs.end()) {
      return input->second.state;
    }
    return AudioDeviceState::DEVICE_NOT_PRESENT;
  }

  std::string GetDefaultID(AudioDeviceDirection direction) {
    EnsureStarted();
    std::scoped_lock lock(mStateMutex);
    return direction == AudioDeviceDirection::OUTPUT ? mDefaultOutput
                                                     : mDefaultInput;
  }

  void SetDefaultID(AudioDeviceDirection direction, const std::string& id) {
    EnsureStarted();
    const auto [deviceName, portName] = SplitDeviceID(id);
    if (deviceName.empty()) {
      return;
    }

    std::string currentDefault;
    {
      std::scoped_lock lock(mStateMutex);
      currentDefault
        = direction == AudioDeviceDirection::OUTPUT ? mDefaultOutput
                                                    : mDefaultInput;
    }
    const auto [currentDevice, currentPort] = SplitDeviceID(currentDefault);
    if (currentDevice == deviceName && currentPort == portName) {
      return;// already active
    }

    const bool output = direction == AudioDeviceDirection::OUTPUT;
    bool issued { false };
    pa_threaded_mainloop_lock(mMainLoop);
    if (pa_context_get_state(mContext) == PA_CONTEXT_READY) {
      issued = true;
      if (currentDevice != deviceName) {
        pa_operation* op = output
          ? pa_context_set_default_sink(
            mContext, deviceName.c_str(), &OnSetSuccess, this)
          : pa_context_set_default_source(
            mContext, deviceName.c_str(), &OnSetSuccess, this);
        if (op) {
          pa_operation_unref(op);
        } else {
          Log("failed to set default %s", output ? "sink" : "source");
        }
      }
      if (!portName.empty() && portName != currentPort) {
        pa_operation* op = output
          ? pa_context_set_sink_port_by_name(
            mContext, deviceName.c_str(), portName.c_str(), &OnSetSuccess, this)
          : pa_context_set_source_port_by_name(
            mContext, deviceName.c_str(), portName.c_str(), &OnSetSuccess, this);
        if (op) {
          pa_operation_unref(op);
        } else {
          Log("failed to set %s port", output ? "sink" : "source");
        }
      }
    } else {
      Log("cannot set device: PulseAudio context is not connected");
    }
    pa_threaded_mainloop_unlock(mMainLoop);

    if (!issued) {
      return;
    }
    // Optimistic update so reads immediately after a set see the new value;
    // the subscription-triggered refresh corrects anything that failed.
    std::scoped_lock lock(mStateMutex);
    if (output) {
      mDefaultOutput = id;
    } else {
      mDefaultInput = id;
    }
  }

  DefaultChangeCallbackHandle AddDefaultCallback(
    std::function<void(AudioDeviceDirection, AudioDeviceRole, const std::string&)>
      callback) {
    auto impl = std::make_shared<DefaultChangeCallbackHandle::Impl>();
    impl->callback = std::move(callback);
    {
      // Register before starting the connection so the initial refresh - and
      // every later one - sees this callback.
      std::scoped_lock lock(mCallbackMutex);
      mCallbacks.emplace_back(impl);
    }
    EnsureStarted();
    return DefaultChangeCallbackHandle(std::move(impl));
  }

 private:
  Backend() = default;

  ~Backend() {
    if (mMainLoop) {
      pa_threaded_mainloop_stop(mMainLoop);
    }
    if (mContext) {
      pa_context_disconnect(mContext);
      pa_context_unref(mContext);
    }
    if (mMainLoop) {
      pa_threaded_mainloop_free(mMainLoop);
    }
  }

  /** Connect on first use; block briefly so early reads see real devices. */
  void EnsureStarted() {
    if (mFirstRefreshDone.load(std::memory_order_acquire)) {
      return;
    }
    std::call_once(mStartOnce, [this] { Start(); });
    std::unique_lock lock(mInitMutex);
    mInitCv.wait_for(
      lock,
      std::chrono::seconds(3),
      [this] { return mFirstRefreshDone.load(std::memory_order_acquire); });
  }

  void Start() {
    mMainLoop = pa_threaded_mainloop_new();
    if (!mMainLoop) {
      Log("pa_threaded_mainloop_new() failed");
      MarkInitDone();
      return;
    }
    mApi = pa_threaded_mainloop_get_api(mMainLoop);
    NewContext();
    if (pa_threaded_mainloop_start(mMainLoop) != 0) {
      Log("pa_threaded_mainloop_start() failed");
      MarkInitDone();
    }
  }

  /** Caller must hold the mainloop lock (or not have started it yet). */
  void NewContext() {
    if (mContext) {
      pa_context_unref(mContext);
    }
    mContext = pa_context_new(mApi, "com.morganscruggs.audioswitcherplus");
    pa_context_set_state_callback(mContext, &OnContextState, this);
    pa_context_set_subscribe_callback(mContext, &OnSubscribeEvent, this);
    if (
      pa_context_connect(mContext, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr)
      < 0) {
      Log(
        "pa_context_connect() failed: %s",
        pa_strerror(pa_context_errno(mContext)));
    }
  }

  void MarkInitDone() {
    {
      std::scoped_lock lock(mInitMutex);
      mFirstRefreshDone.store(true, std::memory_order_release);
    }
    mInitCv.notify_all();
  }

  /** Coalesce bursts of events into one enumeration via a defer event. */
  void ScheduleRefresh() {
    if (!mApi) {
      return;
    }
    if (mDeferEvent) {
      mApi->defer_enable(mDeferEvent, 1);
      return;
    }
    mDeferEvent = mApi->defer_new(mApi, &OnDeferRefresh, this);
  }

  void ScheduleReconnect() {
    if (!mApi || mReconnectTimer) {
      return;
    }
    struct timeval tv;
    pa_timeval_store(&tv, pa_rtclock_now() + 5 * PA_USEC_PER_SEC);
    mReconnectTimer = mApi->time_new(mApi, &tv, &OnReconnectTimer, this);
  }

  /** Fire-and-forget enumeration; results land in CommitRefresh(). */
  void Refresh() {
    if (!mContext || pa_context_get_state(mContext) != PA_CONTEXT_READY) {
      return;
    }
    auto* data = new RefreshData();
    data->backend = this;
    pa_operation* op = pa_context_get_server_info(mContext, &OnServerInfo, data);
    if (op) {
      pa_operation_unref(op);
    }
    op = pa_context_get_sink_info_list(mContext, &OnSinkInfo, data);
    if (op) {
      pa_operation_unref(op);
    }
    op = pa_context_get_source_info_list(mContext, &OnSourceInfo, data);
    if (op) {
      pa_operation_unref(op);
    }
  }

  /** Runs on the PulseAudio mainloop thread once all three lists arrived. */
  void CommitRefresh(RefreshData* data) {
    std::unique_ptr<RefreshData> cleanup(data);

    const auto defaultOutputID = ComputeDefaultID(
      data->defaultSinkName, data->sinkActivePort, data->outputs);
    const auto defaultInputID = ComputeDefaultID(
      data->defaultSourceName, data->sourceActivePort, data->inputs);

    std::optional<std::string> fireOutput;
    std::optional<std::string> fireInput;
    {
      std::scoped_lock lock(mStateMutex);
      mOutputs = std::move(data->outputs);
      mInputs = std::move(data->inputs);
      mDefaultOutput = defaultOutputID;
      mDefaultInput = defaultInputID;
      // Always fire the first time, and afterwards whenever the default
      // (including its active port) changed.
      if (mLastFiredOutput != defaultOutputID) {
        mLastFiredOutput = defaultOutputID;
        fireOutput = defaultOutputID;
      }
      if (mLastFiredInput != defaultInputID) {
        mLastFiredInput = defaultInputID;
        fireInput = defaultInputID;
      }
    }
    MarkInitDone();
    if (fireOutput) {
      Fire(AudioDeviceDirection::OUTPUT, *fireOutput);
    }
    if (fireInput) {
      Fire(AudioDeviceDirection::INPUT, *fireInput);
    }
  }

  static std::string ComputeDefaultID(
    const std::string& defaultName,
    const std::map<std::string, std::string>& activePorts,
    const std::map<std::string, AudioDeviceInfo>& devices) {
    if (defaultName.empty()) {
      return {};
    }
    const auto port = activePorts.find(defaultName);
    if (port != activePorts.end() && !port->second.empty()) {
      const std::string withPort = defaultName + "|" + port->second;
      if (devices.contains(withPort)) {
        return withPort;
      }
    }
    return defaultName;
  }

  /** Must be called without mStateMutex; callbacks may re-enter the API. */
  void Fire(AudioDeviceDirection direction, const std::string& id) {
    std::vector<std::shared_ptr<DefaultChangeCallbackHandle::Impl>> live;
    {
      std::scoped_lock lock(mCallbackMutex);
      mCallbacks.erase(
        std::remove_if(
          mCallbacks.begin(),
          mCallbacks.end(),
          [](const std::weak_ptr<DefaultChangeCallbackHandle::Impl>& wp) {
            return wp.expired();
          }),
        mCallbacks.end());
      for (const auto& weak : mCallbacks) {
        if (auto impl = weak.lock()) {
          live.push_back(std::move(impl));
        }
      }
    }
    for (const auto& impl : live) {
      if (impl->callback) {
        impl->callback(direction, AudioDeviceRole::DEFAULT, id);
      }
    }
  }

  static void AddDevice(
    std::map<std::string, AudioDeviceInfo>& out,
    std::map<std::string, std::string>& activePorts,
    AudioDeviceDirection direction,
    const std::string& name,
    const std::string& description,
    const std::vector<PortInfo>& ports,
    const std::string& activePort) {
    activePorts[name] = activePort;

    const auto make
      = [&out, direction, &name, &description](
          const std::string& portName,
          const std::string& portDescription,
          AudioDeviceState state) {
          AudioDeviceInfo info;
          info.direction = direction;
          info.state = state;
          if (portName.empty()) {
            info.id = name;
            info.interfaceName = description;
            info.endpointName = {};
            info.displayName = description;
          } else {
            info.id = name + "|" + portName;
            info.interfaceName = description + " - " + portDescription;
            info.endpointName = portDescription;
            info.displayName = portDescription + " (" + description + ")";
          }
          out.emplace(info.id, std::move(info));
        };

    if (ports.empty()) {
      make({}, {}, AudioDeviceState::CONNECTED);
      return;
    }

    const bool anyAvailable = std::any_of(
      ports.begin(), ports.end(), [](const PortInfo& port) {
        return port.available != PA_PORT_AVAILABLE_NO;
      });
    if (!anyAvailable) {
      // e.g. HDMI output with the cable unplugged: keep the device visible,
      // but flag it so the plugin/PI treat it as unplugged.
      make({}, {}, AudioDeviceState::DEVICE_PRESENT_NO_CONNECTION);
      return;
    }

    for (const auto& port : ports) {
      make(
        port.name,
        port.description,
        port.available == PA_PORT_AVAILABLE_NO
          ? AudioDeviceState::DEVICE_PRESENT_NO_CONNECTION
          : AudioDeviceState::CONNECTED);
    }
  }

  static std::vector<PortInfo> PortInfos(
    pa_sink_port_info** ports,
    uint32_t count) {
    std::vector<PortInfo> out;
    for (uint32_t i = 0; i < count; ++i) {
      const auto* port = ports ? ports[i] : nullptr;
      if (!port || !port->name) {
        continue;
      }
      out.push_back({
        port->name,
        (port->description && *port->description) ? port->description
                                                  : port->name,
        port->available,
      });
    }
    return out;
  }

  static void DataOneDone(RefreshData* data) {
    bool last = false;
    {
      std::scoped_lock lock(data->mutex);
      last = --data->pending == 0;
    }
    if (last) {
      data->backend->CommitRefresh(data);
    }
  }

  static void OnContextState(pa_context* c, void* userdata) {
    auto* self = static_cast<Backend*>(userdata);
    switch (pa_context_get_state(c)) {
      case PA_CONTEXT_READY:
        Log("connected to %s", pa_context_get_server(c));
        if (auto* op = pa_context_subscribe(
              c,
              static_cast<pa_subscription_mask_t>(
                PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE
                | PA_SUBSCRIPTION_MASK_SERVER),
              &OnSubscribeDone,
              self)) {
          pa_operation_unref(op);
        }
        self->ScheduleRefresh();
        break;
      case PA_CONTEXT_FAILED:
        Log(
          "PulseAudio connection failed: %s",
          pa_strerror(pa_context_errno(c)));
        self->MarkInitDone();
        self->ScheduleReconnect();
        break;
      case PA_CONTEXT_TERMINATED:
        self->MarkInitDone();
        break;
      default:
        break;
    }
  }

  static void OnReconnectTimer(
    pa_mainloop_api* api,
    pa_time_event* event,
    const struct timeval*,
    void* userdata) {
    auto* self = static_cast<Backend*>(userdata);
    api->time_free(event);
    self->mReconnectTimer = nullptr;
    Log("reconnecting to PulseAudio/PipeWire...");
    self->NewContext();
  }

  static void OnDeferRefresh(
    pa_mainloop_api* api,
    pa_defer_event* event,
    void* userdata) {
    auto* self = static_cast<Backend*>(userdata);
    api->defer_enable(event, 0);
    self->Refresh();
  }

  static void OnSubscribeEvent(
    pa_context*,
    pa_subscription_event_type_t type,
    uint32_t index,
    void* userdata) {
    (void)index;
    auto* self = static_cast<Backend*>(userdata);
    const auto facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    if (
      facility == PA_SUBSCRIPTION_EVENT_SINK
      || facility == PA_SUBSCRIPTION_EVENT_SOURCE
      || facility == PA_SUBSCRIPTION_EVENT_SERVER) {
      self->ScheduleRefresh();
    }
  }

  static void OnSubscribeDone(pa_context* c, int success, void* userdata) {
    (void)userdata;
    if (!success) {
      Log("subscribe failed: %s", pa_strerror(pa_context_errno(c)));
    }
  }

  static void OnSetSuccess(pa_context* c, int success, void* userdata) {
    (void)userdata;
    if (!success) {
      Log("set default/port failed: %s", pa_strerror(pa_context_errno(c)));
    }
  }

  static void OnServerInfo(
    pa_context*,
    const pa_server_info* info,
    void* userdata) {
    auto* data = static_cast<RefreshData*>(userdata);
    if (info) {
      data->defaultSinkName
        = info->default_sink_name ? info->default_sink_name : "";
      data->defaultSourceName
        = info->default_source_name ? info->default_source_name : "";
    }
    DataOneDone(data);
  }

  static void OnSinkInfo(
    pa_context*,
    const pa_sink_info* info,
    int eol,
    void* userdata) {
    auto* data = static_cast<RefreshData*>(userdata);
    if (info && info->name) {
      AddDevice(
        data->outputs,
        data->sinkActivePort,
        AudioDeviceDirection::OUTPUT,
        info->name,
        (info->description && *info->description) ? info->description
                                                  : info->name,
        PortInfos(info->ports, info->n_ports),
        (info->active_port && info->active_port->name)
          ? info->active_port->name
          : "");
      return;
    }
    if (eol) {
      DataOneDone(data);
    }
  }

  static void OnSourceInfo(
    pa_context*,
    const pa_source_info* info,
    int eol,
    void* userdata) {
    auto* data = static_cast<RefreshData*>(userdata);
    if (info && info->name) {
      std::vector<PortInfo> ports;
      for (uint32_t i = 0; i < info->n_ports; ++i) {
        const auto* port = info->ports ? info->ports[i] : nullptr;
        if (!port || !port->name) {
          continue;
        }
        ports.push_back({
          port->name,
          (port->description && *port->description) ? port->description
                                                    : port->name,
          port->available,
        });
      }
      AddDevice(
        data->inputs,
        data->sourceActivePort,
        AudioDeviceDirection::INPUT,
        info->name,
        (info->description && *info->description) ? info->description
                                                  : info->name,
        std::move(ports),
        (info->active_port && info->active_port->name)
          ? info->active_port->name
          : "");
      return;
    }
    if (eol) {
      DataOneDone(data);
    }
  }

  pa_threaded_mainloop* mMainLoop { nullptr };
  pa_mainloop_api* mApi { nullptr };
  pa_context* mContext { nullptr };
  pa_defer_event* mDeferEvent { nullptr };
  pa_time_event* mReconnectTimer { nullptr };

  std::once_flag mStartOnce;
  std::atomic<bool> mFirstRefreshDone { false };
  std::mutex mInitMutex;
  std::condition_variable mInitCv;

  std::mutex mStateMutex;
  std::map<std::string, AudioDeviceInfo> mOutputs;
  std::map<std::string, AudioDeviceInfo> mInputs;
  std::string mDefaultOutput;
  std::string mDefaultInput;
  std::optional<std::string> mLastFiredOutput;
  std::optional<std::string> mLastFiredInput;

  std::mutex mCallbackMutex;
  std::vector<std::weak_ptr<DefaultChangeCallbackHandle::Impl>> mCallbacks;
};

}// namespace

std::map<std::string, AudioDeviceInfo> GetAudioDeviceList(
  AudioDeviceDirection direction) {
  return Backend::Get().GetDeviceList(direction);
}

AudioDeviceState GetAudioDeviceState(const std::string& id) {
  return Backend::Get().GetDeviceState(id);
}

std::string GetDefaultAudioDeviceID(
  AudioDeviceDirection direction,
  AudioDeviceRole role) {
  (void)role;// PulseAudio has a single default per direction
  return Backend::Get().GetDefaultID(direction);
}

void SetDefaultAudioDeviceID(
  AudioDeviceDirection direction,
  AudioDeviceRole role,
  const std::string& deviceID) {
  (void)role;
  Backend::Get().SetDefaultID(direction, deviceID);
}

DefaultChangeCallbackHandle AddDefaultAudioDeviceChangeCallback(
  std::function<void(AudioDeviceDirection, AudioDeviceRole, const std::string&)>
    callback) {
  return Backend::Get().AddDefaultCallback(std::move(callback));
}

}// namespace FredEmmott::Audio
