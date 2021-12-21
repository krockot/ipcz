#ifndef IPCZ_SRC_CORE_DRIVER_TRANSPORT_H_
#define IPCZ_SRC_CORE_DRIVER_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "ipcz/ipcz.h"
#include "mem/ref_counted.h"
#include "os/handle.h"
#include "third_party/abseil-cpp/absl/types/span.h"

namespace ipcz {
namespace core {

class DriverTransport : public mem::RefCounted {
 public:
  using Pair = std::pair<mem::Ref<DriverTransport>, mem::Ref<DriverTransport>>;

  struct Descriptor {
    Descriptor();
    Descriptor(Descriptor&&);
    Descriptor& operator=(Descriptor&&);
    ~Descriptor();

    std::vector<uint8_t> data;
    std::vector<os::Handle> handles;
  };

  class Data : public absl::Span<const uint8_t> {
   public:
    Data();
    Data(absl::Span<const uint8_t> data);
    Data(absl::Span<const char> str);

    template <size_t N>
    Data(const char str[N]) : Data(absl::MakeSpan(str)) {}

    absl::Span<const char> AsString() const;
  };

  struct Message {
    Message(Data data);
    Message(Data data, absl::Span<os::Handle> handles);
    Message(const Message&);
    Message& operator=(const Message&);
    ~Message();

    Data data;
    absl::Span<os::Handle> handles;
  };

  class Listener {
   public:
    virtual ~Listener() = default;
    virtual IpczResult OnTransportMessage(const Message& message) = 0;
    virtual void OnTransportError() = 0;
  };

  DriverTransport(const IpczDriver& driver, IpczDriverHandle driver_transport);

  static DriverTransport::Pair CreatePair(const IpczDriver& driver,
                                          IpczDriverHandle driver_node);

  // Set the object handling any incoming message or error notifications. This
  // is only safe to set before Activate() is called, or from within one of the
  // Listener methods when invoked by this DriverTransport (because invocations
  // are mutually exclusive). `listener` must outlive this DriverTransport.
  void set_listener(Listener* listener) { listener_ = listener; }

  // Releases the driver handle so that it's no longer controlled by this
  // DriverTranport.
  IpczDriverHandle Release();

  IpczResult Activate();
  IpczResult Deactivate();
  IpczResult TransmitMessage(const Message& message);

  IpczResult Serialize(std::vector<uint8_t>& data,
                       std::vector<os::Handle>& handles);

  static mem::Ref<DriverTransport> Deserialize(const IpczDriver& driver,
                                               IpczDriverHandle driver_node,
                                               absl::Span<const uint8_t> data,
                                               absl::Span<os::Handle> handles);

  template <typename T>
  IpczResult Transmit(T& message) {
    message.Serialize();
    return TransmitMessage(
        Message(Data(message.params_view()), message.handles_view()));
  }

  template <typename T>
  IpczResult TransmitData(const T& data) {
    return TransmitMessage(Message(Data(absl::MakeSpan(
        reinterpret_cast<const uint8_t*>(&data), sizeof(data)))));
  }

  IpczResult Notify(const Message& message);
  void NotifyError();

 private:
  ~DriverTransport() override;

  const IpczDriver& driver_;
  IpczDriverHandle driver_transport_;

  bool serialized_ = false;
  Listener* listener_ = nullptr;
};

}  // namespace core
}  // namespace ipcz

#endif  // IPCZ_SRC_CORE_DRIVER_TRANSPORT_H_
