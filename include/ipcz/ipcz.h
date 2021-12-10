// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_INCLUDE_IPCZ_IPCZ_H_
#define IPCZ_INCLUDE_IPCZ_IPCZ_H_

// This header is intended to be compilable as C99 as well as c++11 or newer,
// so there's some weak/pseudo-typing going on here.

#include <stddef.h>
#include <stdint.h>

#define IPCZ_NO_FLAGS ((uint32_t)0)

// Helper to clarify flag definitions.
#define IPCZ_FLAG_BIT(bit) ((uint32_t)(1u << bit))

// Opaque handle to an ipcz object.
typedef uint64_t IpczHandle;

// An IpczHandle value which is always invalid. Note that arbitrary non-zero
// values are not necessarily valid either, but zero is never valid.
#define IPCZ_INVALID_HANDLE ((IpczHandle)0)

// Generic result code for all ipcz operations. See IPCZ_RESULT_* values below.
typedef uint32_t IpczResult;

// Specific meaning of each value depends on context, but IPCZ_RESULT_OK always
// indicates success. These values are derived from common status code
// definitions across Google software.
#define IPCZ_RESULT_OK ((IpczResult)0)
#define IPCZ_RESULT_CANCELLED ((IpczResult)1)
#define IPCZ_RESULT_UNKNOWN ((IpczResult)2)
#define IPCZ_RESULT_INVALID_ARGUMENT ((IpczResult)3)
#define IPCZ_RESULT_DEADLINE_EXCEEDED ((IpczResult)4)
#define IPCZ_RESULT_NOT_FOUND ((IpczResult)5)
#define IPCZ_RESULT_ALREADY_EXISTS ((IpczResult)6)
#define IPCZ_RESULT_PERMISSION_DENIED ((IpczResult)7)
#define IPCZ_RESULT_RESOURCE_EXHAUSTED ((IpczResult)8)
#define IPCZ_RESULT_FAILED_PRECONDITION ((IpczResult)9)
#define IPCZ_RESULT_ABORTED ((IpczResult)10)
#define IPCZ_RESULT_OUT_OF_RANGE ((IpczResult)11)
#define IPCZ_RESULT_UNIMPLEMENTED ((IpczResult)12)
#define IPCZ_RESULT_INTERNAL ((IpczResult)13)
#define IPCZ_RESULT_UNAVAILABLE ((IpczResult)14)
#define IPCZ_RESULT_DATA_LOSS ((IpczResult)15)

// Helper to specify explicit struct alignment across C and C++ compilers.
#if defined(__cplusplus)
#define IPCZ_ALIGN(alignment) alignas(alignment)
#elif defined(__GNUC__)
#define IPCZ_ALIGN(alignment) __attribute__((aligned(alignment)))
#elif defined(_MSC_VER)
#define IPCZ_ALIGN(alignment) __declspec(align(alignment))
#else
#error "IPCZ_ALIGN() is not defined for your compiler."
#endif

// Helper to generate the smallest constant value which is aligned with
// `alignment` and at least as large as `value`.
#define IPCZ_ALIGNED(value, alignment) \
  ((((value) + ((alignment)-1)) / (alignment)) * (alignment))

// Enumeration which specifies the kind of value held by an IpczOSHandle struct.
// These refer to handles allocated by the application through OS APIs, not
// objects created by ipcz.
typedef uint32_t IpczOSHandleType;

// A POSIX file descriptor. Supported everywhere except Windows.
#define IPCZ_OS_HANDLE_FILE_DESCRIPTOR ((IpczOSHandleType)0)

// A Windows HANDLE. Supported only on Windows.
#define IPCZ_OS_HANDLE_WINDOWS ((IpczOSHandleType)1)

// A Mach send right. Supported only on macOS.
#define IPCZ_OS_HANDLE_MACH_SEND_RIGHT ((IpczOSHandleType)2)

// A Mach receive right. Supported only on macOS.
#define IPCZ_OS_HANDLE_MACH_RECEIVE_RIGHT ((IpczOSHandleType)3)

// A Fuchsia handle. Supported only on Fuchsia.
#define IPCZ_OS_HANDLE_FUCHSIA ((IpczOSHandleType)4)

// Describes a platform-specific handle to an OS resource. Some ipcz functions
// accept specific kinds of OS object handles to consume or manipulate, and this
// is the structure used to represent them.
struct IPCZ_ALIGN(8) IpczOSHandle {
  // The exact size of this structure in bytes. Must be set accurately in any
  // instance of this structure before passing it to an ipcz function.
  uint32_t size;

  // The type of this handle.
  IpczOSHandleType type;

  // An opaque representation of the OS handle. Meaning depends on the value of
  // `type`. Depending on platform, this may be, for example, a zero-extended
  // file descriptor value, a Windows HANDLE value, a macOS mach_port_t, etc.
  uint64_t value;
};

// Describes an opaque platform-specific handle to an OS-level process.
struct IPCZ_ALIGN(8) IpczOSProcessHandle {
  // The exact size of this structure in bytes. Must be set accurately in any
  // instance of this structure before passing it to an ipcz function.
  uint32_t size;

  // The process handle's value. For Windows this is a valid process HANDLE
  // value or pseudohandle. For Fuchsia it must be a zx_handle_t identifying a
  // process object, and for all other (POSIX-compatible) systems it must be a
  // PID (i.e. pid_t).
  uint64_t value;
};

// An opaque handle value created by an IpczDriver implementation. ipcz uses
// such handles to provide relevant context when calling back into the driver.
typedef uint64_t IpczDriverHandle;

#define IPCZ_INVALID_DRIVER_HANDLE ((IpczDriverHandle)0)

// Flags given to the ipcz activity handler by a driver transport to notify ipcz
// about incoming data or state changes.
typedef uint32_t IpczTransportActivityFlags;

// If set, the driver encountered an uncoverable error using the transport and
// ipcz should discard it.
#define IPCZ_TRANSPORT_ACTIVITY_ERROR IPCZ_FLAG_BIT(0)

// If set, the driver is done using the ipcz transport and will no longer invoke
// its activity handler. Driver transports must call this at some point to allow
// ipcz to free associated resources.
#define IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED IPCZ_FLAG_BIT(1)

extern "C" {

// Notifies ipcz of activity on a transport. `transport` must be a handle to a
// transport which is currently activated. This handle is acquired exclusively
// by the driver transport via an ipcz call to the driver's ActivateTransport(),
// which also provides this handler to the driver.
//
// The driver must use this to feed incoming data and OS handles from the
// transport to ipcz, or to inform ipcz of any error conditions resulting in
// unexpected and irrecoverable dysfunction of the transport.
//
// If the driver encounters an unrecoverable error while performing I/O on the
// transport, it should invoke this with the IPCZ_TRANSPORT_ACTIVITY_ERROR flag
// to instigate immediate destruction of the transport.
typedef IpczResult (*IpczTransportActivityHandler)(
    IpczHandle transport,
    const uint8_t* data,
    uint32_t num_bytes,
    const struct IpczOSHandle* os_handles,
    uint32_t num_os_handles,
    IpczTransportActivityFlags flags,
    const void* options);

// IpczDriver is a function table to be populated by the application and
// provided to ipcz when creating a new node. The driver implements concrete
// I/O operations to facilitate communication between nodes, giving embedding
// systems full control over choice of OS-specific transport mechanisms and I/O
// scheduling decisions.
//
// The driver API is meant to be used by both the application embedding ipcz,
// particularly for creating transports to make initial contact between nodes,
// as well as by ipcz itself to delegate creation and management of new
// transports which ipcz brokers between nodes.
struct IPCZ_ALIGN(8) IpczDriver {
  // The exact size of this structure in bytes. Must be set accurately by the
  // application before passing this structure to any ipcz API functions.
  uint32_t size;

  // Creates a new pair of entnagled bidirectional transports, returning them in
  // `first_transport` and `second_transport`. Implementation of the transport
  // is up to the driver, but:
  //
  //  - interconnecting nodes must use compatible driver implementations
  //
  //  - in a multiprocess environment, transports must be capable of
  //    transmitting data and OS handles across a process boundary
  //
  //  - the handles and data comprising each transport should be fully
  //    sufficient to operate the transport from another node if those handles
  //    and data are moved there
  //
  //  - a transport is only activated once, and once it's activated it will
  //    never be moved to another node
  //
  //  - once a transport is released, it is never re-activated by ipcz
  //
  // Transports created by this call are not necessarily used by the calling
  // node, so the driver must not assume ownership or responsibility for them.
  //
  // `driver_node` is the application-provided driver-side handle assigned to
  // the node when created with CreateNode().
  //
  // Returns IPCZ_RESULT_OK on success. Any other return value indicates
  // failure.
  IpczResult (*CreateTransports)(IpczDriverHandle driver_node,
                                 uint32_t flags,
                                 const void* options,
                                 IpczDriverHandle* first_transport,
                                 IpczDriverHandle* second_transport);

  // Called by ipcz to request that the driver cease activity on the transport
  // corresponding to `driver_transport` and clean up any associated resources.
  // Once this is returns, both `driver_transport` and its corresponding
  // `transport` IpczHandle (associated via ActivateTransport()) are
  // invalidated.
  IpczResult (*DestroyTransport)(IpczDriverHandle driver_transport,
                                 uint32_t flags,
                                 const void* options);

  // Serializes a driver transport into a collection of bytes and handles which
  // can be used to relocate it to another node -- possibly in another
  // process -- where it can be deserialized by DeserializeTransport().
  //
  // On input, `*num_bytes` and `*num_os_handles` specify the amount of storage
  // available in `data` and `os_handles` respectively. If insufficient to store
  // the full serialized output, this returns IPCZ_RESULT_RESOURCE_EXHAUSTED.
  //
  // In both success and failure cases, `*num_bytes` and `*num_os_handles` are
  // updated with the exact amount of storage required for each before
  // returning.
  //
  // If the caller's provided storage is sufficent, `data` and `os_handles` will
  // be populated with the serialized transport, and this returns
  // IPCZ_RESULT_OK. In this case `driver_transport` is also invalidated.
  //
  // Drivers are allowed but not required to support serialization of a
  // transport which has already been activated by ActivateTransport(). If
  // SerializeTransport() is called on such a transport, the driver may return
  // IPCZ_RESULT_FAILED_PRECONDITION.
  IpczResult (*SerializeTransport)(IpczDriverHandle driver_transport,
                                   uint32_t flags,
                                   const void* options,
                                   uint8_t* data,
                                   uint32_t* num_bytes,
                                   struct IpczOSHandle* os_handles,
                                   uint32_t* num_os_handles);

  // Deserializes a driver transport from a collection of bytes and handles
  // which was originally produced by SerializeTransport(). The transport must
  // not be activated before this returns.
  //
  // `driver_node` is the application-provided driver-side handle assigned to
  // the node when created with CreateNode().
  //
  // If ipcz has a known process handle to the remote process on the other end
  // of the transport, it's provided to the driver in `target_process`.
  // Otherwise `target_process` is null.
  //
  // Any return value other than IPCZ_RESULT_OK indicates an error, and the
  // transport will be dropped by ipcz. Otherwise ipcz will imminently activate
  // the transport via a call to the driver's ActivateTransport().
  IpczResult (*DeserializeTransport)(
      IpczDriverHandle driver_node,
      const uint8_t* data,
      uint32_t num_bytes,
      const IpczOSHandle* os_handles,
      uint32_t num_os_handles,
      const struct IpczOSProcessHandle* target_process,
      uint32_t flags,
      const void* options,
      IpczDriverHandle* driver_transport);

  // Called by ipcz to activate a transport. `driver_transport` is the
  // driver-side handle assigned to the transport by the driver, either as given
  // to ipcz via ConnectNode(), or as returned by the driver from an ipcz call
  // out to CreateDriverTransport().
  //
  // `transport` is a handle the driver can use when calling `activity_handler`
  // to update ipcz regarding any incoming data or state changes from the
  // transport.
  //
  // Before this returns, the driver should establish any I/O monitoring or
  // scheduling state necessary to support operation of the endpoint, and once
  // it returns ipcz may immediately begin making Transmit() calls on
  // `driver_transport`.
  //
  // Any return value other than IPCZ_RESULT_OK indicates an error, and the
  // endpoint will be dropped by ipcz. Otherwise the endpoint may be used
  // immediately to accept or submit data, and it should continue to operate
  // until ipcz calls DestroyDriverTransport() on `driver_transport`.
  //
  // Note that `activity_handler` invocations MUST be mutually exclusive,
  // because transmissions from ipcz are expected to arrive and be processed
  // strictly in-order.
  //
  // The driver may elicit forced destruction of itself by calling
  // `activity_handler` with the flag IPCZ_TRANSPORT_ACTIVITY_DEACTIVATED.
  IpczResult (*ActivateTransport)(IpczDriverHandle driver_transport,
                                  IpczHandle transport,
                                  IpczTransportActivityHandler activity_handler,
                                  uint32_t flags,
                                  const void* options);

  // Called by ipcz to deactivate a transport. Once this returns successfully,
  // the driver must make no further calls into this transport's activity
  // handler. ipcz may continue to use the transport for outgoing transmissions
  // until DestroyTransport() is called.
  IpczResult (*DeactivateTransport)(IpczDriverHandle driver_transport,
                                    uint32_t flags,
                                    const void* options);

  // Called by ipcz to delegate transmission of data and OS handles over the
  // identified transport endpoint. If the driver cannot fulfill the request,
  // it must return a result other than IPCZ_RESULT_OK, and this will cause the
  // transport's connection to be severed.
  //
  // The net result of this transmission should be an activity handler
  // invocation on the correpsonding remote transport by the driver on its node.
  // It is the driver's responsibility to get any data and handles to the other
  // transport, and to ensure that all transmissions from transport end up
  // invoking the activity handler on the peer transport in the same order they
  // were transmitted.
  //
  // If ipcz only wants to wake the peer node rather than transmit data or
  // handles, `num_bytes` and `num_os_handles` may both be zero.
  IpczResult (*Transmit)(IpczDriverHandle driver_transport,
                         const uint8_t* data,
                         uint32_t num_bytes,
                         const struct IpczOSHandle* os_handles,
                         uint32_t num_os_handles,
                         uint32_t flags,
                         const void* options);
};

}  // extern "C"

// See CreateNode() and the IPCZ_CREATE_NODE_* flag descriptions below.
typedef uint32_t IpczCreateNodeFlags;

// Indicates that the created node will serve as the broker in its cluster.
//
// Brokers are expected to live in relatively trusted processes -- not elevated
// in privelege but also generally not restricted by sandbox constraints and not
// prone to processing risky, untrusted data -- as they're responsible for
// helping other nodes establish direct lines of communication, as well as in
// some cases facilitating proxying of data and relaying of OS handles.
//
// Broker nodes do not expose any additional ipcz APIs or require much other
// special care on the part of the application**, but every cluster of connected
// nodes must have a node designated as the broker. Typically this is the first
// node created by an application's main process or a system-wide service
// coordinator, and all other nodes are created in processes spawned by that one
// or in processes which otherwise trust it.
//
// ** See notes on DestroyNode() regarding destruction of broker nodes.
#define IPCZ_CREATE_NODE_AS_BROKER IPCZ_FLAG_BIT(0)

// See ConnectNode() and the IPCZ_CONNECT_NODE_* flag descriptions below.
typedef uint32_t IpczConnectNodeFlags;

// Indicates that the remote node for this connection is expected to be a broker
// node, and it will be treated as such. Do not use this flag when connecting to
// any untrusted process.
#define IPCZ_CONNECT_NODE_TO_BROKER IPCZ_FLAG_BIT(0)

// Optional limits provided by IpczPutOptions for Put() or IpczBeginPutOptions
// for BeginPut().
struct IPCZ_ALIGN(8) IpczPutLimits {
  // The exact size of this structure in bytes. Must be set accurately before
  // passing the structure to any API functions.
  uint32_t size;

  // Specifies the maximum number of unread parcels to allow in a portal's
  // queue. If a Put() or BeginPut() call specifying this limit would cause the
  // receiver's number of number of queued unread parcels to exceed this value,
  // the call will fail with IPCZ_RESULT_RESOURCE_EXHAUSTED.
  uint32_t max_queued_parcels;

  // Specifies the maxmimum number of data bytes to allow in a portal's queue.
  // If a Put() or BeginPut() call specifying this limit would cause the number
  // of data bytes across all queued unread parcels to exceed this value, the
  // call will fail with IPCZ_RESULT_RESOURCE_EXHAUSTED.
  uint32_t max_queued_bytes;
};

// Options given to Put() to modify its default behavior.
struct IPCZ_ALIGN(8) IpczPutOptions {
  // The exact size of this structure in bytes. Must be set accurately before
  // passing the structure to Put().
  uint32_t size;

  // Optional limits to apply when determining if the Put() should be completed.
  const IpczPutLimits* limits;
};

// See BeginPut() and the IPCZ_BEGIN_PUT_* flags described below.
typedef uint32_t IpczBeginPutFlags;

// Indicates that the caller is willing to produce less data than originally
// requested by their `*num_bytes` argument to BeginPut(). If the implementation
// would prefer a smaller chunk of data or if the requested size would exceed
// limits specified in the call's corresponding IpczPutLimits, passing this flag
// may allow the call to succeed while returning a smaller acceptable value in
// `*num_bytes`, rather than simply failing the call with
// IPCZ_RESULT_RESOURCE_EXHAUSTED.
#define IPCZ_BEGIN_PUT_ALLOW_PARTIAL IPCZ_FLAG_BIT(0)

// Options given to BeginPut() to modify its default behavior.
struct IPCZ_ALIGN(8) IpczBeginPutOptions {
  // The exact size of this structure in bytes. Must be set accurately before
  // passing the structure to BeginPut().
  uint32_t size;

  // Optional limits to apply when determining if the BeginPut() should be
  // completed.
  const IpczPutLimits* limits;
};

// See EndPut() and the IPCZ_END_PUT_* flags described below.
typedef uint32_t IpczEndPutFlags;

// If this flag is given to EndPut(), any in-progress two-phase put operation is
// aborted without committing any data, portals, or OS handles to the portal.
#define IPCZ_END_PUT_ABORT IPCZ_FLAG_BIT(0)

// See EndGet() and the IPC_END_GET_* flag descriptions below.
typedef uint32_t IpczEndGetFlags;

// If this flag is given to EndGet(), any in-progress two-phase get operation is
// aborted without consuming any data from the portal.
#define IPCZ_END_GET_ABORT IPCZ_FLAG_BIT(0)

// See DestroyTrap() and the IPCZ_DESTROY_TRAP_* flags described below.
typedef uint32_t IpczDestroyTrapFlags;

// If this flag is given to DestroyTrap(), the call will block until any
// concurrently executing invocation of the trap's event handler are finished.
// See notes on DestroyTrap().
#define IPCZ_DESTROY_TRAP_BLOCKING IPCZ_FLAG_BIT(0)

// Flags given by the `flags` field in IpczPortalStatus.
typedef uint32_t IpczPortalStatusFlags;

// Indicates that the opposite portal is closed. Subsequent put operations on
// this portal will always fail with IPCZ_RESULT_NOT_FOUND. If there are not
// currently any unretrieved parcels in the portal either, subsequent get
// operations will also fail with the same error.
#define IPCZ_PORTAL_STATUS_PEER_CLOSED IPCZ_FLAG_BIT(0)

// Indicates that the opposite portal is closed AND no more parcels can be
// expected to arrive from it. If this bit is set on a portal's status, the
// portal is essentially useless.
#define IPCZ_PORTAL_STATUS_DEAD IPCZ_FLAG_BIT(1)

// Information returned by QueryPortalStatus() or provided to
// IpczTrapEventHandlers when a trap's conditions on their portal.
struct IPCZ_ALIGN(8) IpczPortalStatus {
  // The exact size of this structure in bytes. Must be set accurately before
  // passing the structure to any functions.
  uint32_t size;

  // Flags. See the IPCZ_PORTAL_STATUS_* flags described above for the possible
  // flags combined in this value.
  IpczPortalStatusFlags flags;

  // The number of unretrieved parcels queued on this portal.
  uint32_t num_local_parcels;

  // The number of unretrieved bytes (across all unretrieved parcels) queued on
  // this portal.
  uint32_t num_local_bytes;

  // The number of unretrieved parcels queued on the opposite portal.
  uint32_t num_remote_parcels;

  // The number of unretrieved bytes (across all unretrieved parcels) queued on
  // the opposite portal.
  uint32_t num_remote_bytes;
};

// Flags given to IpczTrapConditions to indicate which types of conditions a
// trap should observe.
typedef uint32_t IpczTrapConditionFlags;

// Triggers a trap event whenever the opposite portal is closed. Typically
// applications are interested in the more specific IPCZ_TRAP_CONDITION_DEAD.
#define IPCZ_TRAP_CONDITION_PEER_CLOSED IPCZ_FLAG_BIT(1)

// Triggers a trap event whenever there are no more parcels available to
// retrieve from this portal AND the opposite portal is closed. This means the
// portal will never again have parcels to retrieve and is effectively useless.
#define IPCZ_TRAP_CONDITION_DEAD IPCZ_FLAG_BIT(2)

// Triggers a trap event whenever the number of parcels queued for retrieval by
// this portal meets or exceeds the threshold given by `min_local_parcels` in
// IpczTrapConditions.
#define IPCZ_TRAP_CONDITION_LOCAL_PARCELS IPCZ_FLAG_BIT(3)

// Triggers a trap event whenever the number of bytes queued for retrieval by
// this portal meets or exceeds the threshold given by `min_local_bytes` in
// IpczTrapConditions.
#define IPCZ_TRAP_CONDITION_LOCAL_BYTES IPCZ_FLAG_BIT(4)

// Triggers a trap event whenever the number of parcels queued for retrieval on
// the opposite portal drops below the threshold given by `max_remote_parcels`
// in IpczTrapConditions.
#define IPCZ_TRAP_CONDITION_REMOTE_PARCELS IPCZ_FLAG_BIT(5)

// Triggers a trap event whenever the number of bytes queued for retrieval on
// the opposite portal drops below the threshold given by `max_remote_bytes` in
// in IpczTrapConditions.
#define IPCZ_TRAP_CONDITION_REMOTE_BYTES IPCZ_FLAG_BIT(6)

// A structure describing portal conditions necessary to trigger a trap and
// invoke its event handler.
struct IPCZ_ALIGN(8) IpczTrapConditions {
  // The exact size of this structure in bytes. Must be set accurately before
  // passing the structure to CreateTrap() or ArmTrap().
  uint32_t size;

  // See the IPCZ_TRAP_CONDITION_* flags described above.
  IpczTrapConditionFlags flags;

  // See IPCZ_TRAP_CONDITION_LOCAL_PARCELS. If that flag is not set in
  // `flags`, this field is ignored.
  uint32_t min_local_parcels;

  // See IPCZ_TRAP_CONDITION_LOCAL_BYTES. If that flag is not set in
  // `flags`, this field is ignored.
  uint32_t min_local_bytes;

  // See IPCZ_TRAP_CONDITION_REMOTE_PARCELS. If that flag is not set in
  // `flags`, this field is ignored.
  uint32_t max_remote_parcels;

  // See IPCZ_TRAP_CONDITION_REMOTE_BYTES. If that flag is not set in
  // `flags`, this field is ignored.
  uint32_t max_remote_bytes;
};

// Structure passed to each IpczTrapEventHandler invocation with details about
// the event.
struct IPCZ_ALIGN(8) IpczTrapEvent {
  // The size of this structure in bytes. Populated by ipcz to indicate which
  // version is being provided to the handler.
  uint32_t size;

  // The context value originally given to CreateTrap() when creating the trap
  // which fired this event.
  uint64_t context;

  // Flags indicating which condition(s) triggered this event.
  IpczTrapConditionFlags condition_flags;

  // The current status of the portal which triggered this event.
  const IpczPortalStatus* status;
};

// An application-defined function to be invoked by a trap when its observed
// conditions are satisfied on the monitored portal.
typedef void (*IpczTrapEventHandler)(const struct IpczTrapEvent* event);

extern "C" {

// Table of API functions defined by ipcz. Instances of this structure may be
// populated by passing them to IpczGetAPI().
//
// Note that all functions follow a consistent parameter ordering:
//
//   1. Object handle (node or portal) if applicable
//   2. Function-specific strict input values
//   3. Flags - possibly untyped and unused
//   4. Options struct - possibly untyped and unused
//   5. Function-specific in/out values
//   6. Function-specific strict output values
//
// The rationale behind this convention is generally to have order flow from
// input to output. Flags are inputs, and options provide an extension point for
// future versions of these APIs; as such they skirt the boundary between strict
// input values and in/out values.
//
// The order and signature (ABI) of functions defined here must never change,
// but new functions may be added to the end.
struct IPCZ_ALIGN(8) IpczAPI {
  // The exact size of this structure in bytes. Must be set accurately by the
  // application before passing the structure to IpczGetAPI().
  uint32_t size;

  // Initializes a new ipcz node. Applications typically need only one node in
  // each communicating process, but it's OK to create more. Practical use cases
  // for multiple nodes per process may include various testing scenarios, and
  // any situation where simulating a multiprocess environment is useful.
  //
  // All other ipcz calls are scoped to a specific node, or to a more specific
  // object which is itself scoped to a specific node.
  //
  // `driver` is the driver to use when coordinating internode communication.
  // Nodes which will be interconnected must use the same or compatible driver
  // implementations.
  //
  // `driver_node` is a driver-side handle to assign to the node throughout its
  // lifetime. This handle provides the driver with additional context when ipcz
  // makes driver API calls pertaining to a specific node.
  //
  // If `flags` contains IPCZ_CREATE_NODE_AS_BROKER then the node will act as
  // the broker in its cluster of connected nodes. See details on that flag
  // description above.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if a new node was created. In this case, `*node` is
  //        populated with a valid node handle upon return.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `node` is null, or `driver` is null or
  //        invalid.
  IpczResult (*CreateNode)(const struct IpczDriver* driver,
                           IpczDriverHandle driver_node,
                           IpczCreateNodeFlags flags,
                           const void* options,
                           IpczHandle* node);

  // Destroys an ipcz node. Explicit destruction is not strictly necessary,
  // but applications and tests may wish to create multiple nodes in the same
  // process, and in cases where a node will no longer be used it's a good idea
  // to destroy it so it can release any allocated resources.
  //
  // This function is NOT thread-safe. It is the application's responsibility to
  // ensure that no other threads are making ipcz calls on `node` concurrently
  // with this call, or any time thereafter since `node` will no longer be
  // valid.
  //
  // Once this call returns, ipcz will never invoke another driver API with this
  // node's associated IpczDriverHandle assigned by the original CreateNode()
  // call, assuming the assigned driver handle was unique to this node.
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // NOTE: If `node` is the broker node in its cluster of connected nodes,
  // certain operations across the cluster -- such as handle transmission
  // through portals or portal transferrence in general -- may begin to fail
  // spontaneously once destruction is complete.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if `node` was destroyed.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `node` is invalid.
  IpczResult (*DestroyNode)(IpczHandle node,
                            uint32_t flags,
                            const void* options);

  // Connects `node` to another node in the system using an application-provided
  // driver transport handle in `driver_transport` for communication. If this
  // call will succeed, ipcz will call back into the driver to activate this
  // transport via ActivateTransport() before returning.
  //
  // The application is responsible for delivering the other endpoint of the
  // transport to whatever other node will use it with its own corresponding
  // ConnectNode() call.
  //
  // If the caller has a process handle to the process in which the other node
  // lives, it should be provided in `target_process`. If `node` is a broker
  // node, a valid process handle may be required on Windows for the transport
  // to be fully operational.
  //
  // If IPCZ_CONNECT_NODE_TO_BROKER is given in `flags`, the remote node must
  // be a broker node, and the calling node will treat it as such.
  //
  // The caller may establish any number of initial portals to be linked
  // between the nodes as soon as the two-way connection is complete. On
  // success, all returned portal handles are usable immediately by the
  // application.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if all arguments were valid and connection was initiated.
  //        `num_portals` portal handles are populated in `portals` and can be
  //        used immediately by the application.
  //
  //        Note that because connection is generally an asynchronous operation
  //        it may still fail after this returns. If connection fails in this
  //        case, any returned initial portals will eventually appear to have a
  //        closed peer.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `node` is invalid, `num_initial_portals`
  //        is zero, or `initial_portals` is null.
  IpczResult (*ConnectNode)(IpczHandle node,
                            IpczDriverHandle driver_transport,
                            const struct IpczOSProcessHandle* target_process,
                            uint32_t num_initial_portals,
                            IpczConnectNodeFlags flags,
                            const void* options,
                            IpczHandle* initial_portals);

  // Opens two new portals which exist as each other's opposite.
  //
  // Data, other portals, and OS handles can be put in a portal with put
  // operations (see Put(), BeginPut(), EndPut()). Anything placed into a portal
  // can be retrieved in the same order by get operations (Get(), BeginGet(),
  // EndGet()) on the opposite portal.
  //
  // To open portals which span two different nodes at creation time, see
  // ConnectNode().
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if portal creation was successful. `*portal0` and
  //        `*portal1` are each populated with opaque portal handles which
  //        identify the new pair of portals. The new portals are each other's
  //        opposite and are entangled until one of them is closed.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `node` is invalid, or if either
  //        `portal0` or `portal1` is null.
  IpczResult (*OpenPortals)(IpczHandle node,
                            uint32_t flags,
                            const void* options,
                            IpczHandle* portal0,
                            IpczHandle* portal1);

  // Closes the portal identified by `portal`.
  //
  // This function is NOT thread-safe. It is the application's responsibility to
  // ensure that no other threads are performing other operations on `portal`
  // concurrently with this call or any time thereafter.
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if `portal` referred to a valid portal in `node` and
  //        was successfully closed by this operation.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid.
  IpczResult (*ClosePortal)(IpczHandle portal,
                            uint32_t flags,
                            const void* options);

  // Merges two portals into each other, effectively destroying both while
  // linking their respective peer portals with each other. A portal cannot
  // merge with its own peer, and a portal cannot be merged into another if one
  // or more parcels have already been put into or taken out of it. There are
  // however no restrictions on what can be done to the portal's peer prior to
  // merging the portal with another.
  //
  // If we have two portal pairs:
  //
  //    A ---- B     and    C ---- D
  //
  // some parcels are placed into A, and some parcels are placed into D, and
  // then we merge B with C, the net result will be a single portal pair:
  //
  //    A ---- D
  //
  // All past and future parcels placed into A will arrive at D, and vice versa.
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the two portals were merged successfully. Neither
  //        handle is valid past this point. Parcels now travel between the
  //        merged portals' respective peers, including any parcels that were
  //        in flight or queued at the time of this merge.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `first` or `second` is invalid, or if
  //        `first` and `second` are each others' peer.
  //
  //    IPCZ_RESULT_FAILED_PRECONDITION if either `first` or `second` has
  //        already had one or more parcels put into or gotten out of them.
  IpczResult (*MergePortals)(IpczHandle first,
                             IpczHandle second,
                             uint32_t flags,
                             const void* options);

  // Queries specific details regarding the status of a portal, such as the
  // number of unread parcels or data bytes available on the portal or its
  // opposite, or whether the opposite portal has already been closed.
  //
  // Note that because the portal's status is inherently dynamic and may be
  // modified at any time by any thread in any process with a handle to either
  // the portal or its opposite, the information returned in `status` may be
  // stale by the time a successful QueryPortalStatus() call returns.
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the requested query was completed successfully.
  //        `status` is populated with details.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT `portal` is invalid. `status` is null or
  //        invalid.
  IpczResult (*QueryPortalStatus)(IpczHandle portal,
                                  uint32_t flags,
                                  const void* options,
                                  struct IpczPortalStatus* status);

  // Puts any combination of raw data, portals, and OS handles into the portal
  // identified by `portal`. Everything put into a portal can be retrieved in
  // the same order by a corresponding get operation on the opposite portal.
  //
  // `flags` is unused and must be IPCZ_NO_FLAGS.
  //
  // `options` may be null.
  //
  // If this call fails (returning anything other than IPCZ_RESULT_OK), any
  // provided portals or OS handles remain property of the caller. If it
  // succeeds, their ownership is assumed by ipcz.
  //
  // Data to be submitted is read directly from the address given by the `data`
  // argument, and `num_bytes` specifies how many bytes of data to copy from
  // there.
  //
  // Callers may wish to request a view directly into portal memory for direct
  // writing (for example, in cases where copying first from some other source
  // into a separate application buffer just for Put() would be redundant.) In
  // such cases, a two-phase put operation can instead be used by calling
  // BeginPut() and EndPut() as defined below.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the provided data, portals, and OS handles were
  //        successfully placed into the portal as a new parcel.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `data` is null but
  //        `num_bytes` is non-zero, `portals` is null but `num_portals` is
  //        non-zero, `os_handles` is null but `num_os_handles` is non-zero,
  //        `options` is non-null but invalid, or one of the portals in
  //        `portals` is equal to `portal` its (local) opposite if applicable.
  //
  //    IPCZ_RESULT_RESOURCE_EXHAUSTED if `options->limits` is non-null and at
  //        least one of the specified limits would be violated by the
  //        successful completion of this call.
  //
  //    IPCZ_RESULT_NOT_FOUND if it is known that the opposite portal has
  //        already been closed and anything put into this portal would be lost.
  //
  //    IPCZ_RESULT_PERMISSION_DENIED if the caller attempted to place handles
  //        into the portal which could not be transferred to the other side due
  //        to OS-level privilege constraints.
  IpczResult (*Put)(IpczHandle portal,
                    const void* data,
                    uint32_t num_bytes,
                    const IpczHandle* portals,
                    uint32_t num_portals,
                    const struct IpczOSHandle* os_handles,
                    uint32_t num_os_handles,
                    uint32_t flags,
                    const struct IpczPutOptions* options);

  // Begins a two-phase put operation on `portal`. While a two-phase put
  // operation is in progress on a portal, all other put operations on the same
  // portal will fail with IPCZ_RESULT_ALREADY_EXISTS.
  //
  // Unlike a plain Put() call, two-phase put operations allow the application
  // to write directly into portal memory, potentially reducing memory access
  // costs by eliminating redundant copying and caching.
  //
  // The input value of `*num_bytes` tells ipcz how much data the caller would
  // like to place into the portal.
  //
  // Limits provided to BeginPut() elicit similar behavior to Put(), with the
  // exception that `flags` may specify IPCZ_BEGIN_PUT_ALLOW_PARTIAL to allow
  // BeginPut() to succeed even the caller's suggested value of
  // `*num_bytes` would cause the portal to exceed the maximum queued byte limit
  // given by `options->limits`. In that case BeginPut() may update `*num_bytes`
  // to reflect the remaining capacity of the portal, allowing the caller to
  // commit at least some portion of their data with EndPut().
  //
  // Handles for two-phase puts are only provided when finalizing the operation
  // with EndPut().
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the two-phase put operation has been successfully
  //        initiated. This operation must be completed with EndPut() before any
  //        further Put() or BeginPut() calls are allowed on `portal`. `*data`
  //        is set to the address of a portal buffer into which the application
  //        may copy its data, and `*num_bytes` is updated to reflect the
  //        capacity of that buffer, which may be greater than (or less than, if
  //        and only if IPCZ_BEGIN_PUT_ALLOW_PARTIAL was set in `flags`) the
  //        capacity requested by the input value of `*num_bytes`.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `*num_bytes` is
  //        non-zero but `data` is null, or options is non-null and invalid.
  //
  //    IPCZ_RESULT_RESOURCE_EXHAUSTED if completing the put with the number of
  //        bytes specified by `*num_bytes` would cause the portal to exceed the
  //        queued parcel limit or (if IPCZ_BEGIN_PUT_ALLOW_PARTIAL is not
  //        specified in `flags`) data byte limit specified by
  //        `options->limits`.
  //
  //    IPCZ_RESULT_ALREADY_EXISTS if there is already a two-phase put operation
  //        in progress on `portal`.
  //
  //    IPCZ_RESULT_NOT_FOUND if it is known that the opposite portal has
  //        already been closed and anything put into this portal would be lost.
  IpczResult (*BeginPut)(IpczHandle portal,
                         IpczBeginPutFlags flags,
                         const struct IpczBeginPutOptions* options,
                         uint32_t* num_bytes,
                         void** data);

  // Ends the two-phase put operation started by the most recent successful call
  // to BeginPut() on `portal`.
  //
  // `num_bytes_produced` specifies the number of bytes actually written into
  // the buffer that was returned from the original BeginPut() call.
  //
  // Usage of `portals`, `num_portals`, `os_handles`, and `num_os_handles` is
  // identical to Put().
  //
  // If this call fails (returning anything other than IPCZ_RESULT_OK), any
  // provided portals or OS handles remain property of the caller. If it
  // succeeds, their ownership is assumed by ipcz.
  //
  // If IPCZ_END_PUT_ABORT is given in `flags` and there is a two-phase put
  // operation in progress on `portal`, all other arguments are ignored and the
  // pending two-phase put operation is cancelled without committing a new
  // parcel to the portal.
  //
  // If EndPut() fails for any reason other than
  // IPCZ_RESULT_FAILED_PRECONDITION, the two-phase put operation remains in
  // progress, and EndPut() must be called again to abort the operation or
  // attempt completion with different arguments.
  //
  // `options` is unused and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the two-phase operation was successfully completed or
  //        aborted. If not aborted, all data, portals, and OS handles were
  //        committed to a new parcel enqueued for retrieval by the opposite
  //        portal.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `num_portals` is
  //        non-zero but `portals` is null, `num_os_handles` is non-zero but
  //        `os_handles` is null, or `num_bytes_produced` is larger than the
  //        capacity of the buffer originally returned by BeginPut().
  //
  //    IPCZ_RESULT_FAILED_PRECONDITION if there was no two-phase put operation
  //        in progress on `portal`.
  //
  //    IPCZ_RESULT_NOT_FOUND if it is known that the opposite portal has
  //        already been closed and anything put into this portal would be lost.
  //
  //    IPCZ_RESULT_PERMISSION_DENIED if the caller attempted to place handles
  //        into the portal which could not be transferred to the other side due
  //        to OS-level privilege constraints.
  IpczResult (*EndPut)(IpczHandle portal,
                       uint32_t num_bytes_produced,
                       const IpczHandle* portals,
                       uint32_t num_portals,
                       const struct IpczOSHandle* os_handles,
                       uint32_t num_os_handles,
                       IpczEndPutFlags flags,
                       const void* options);

  // Retrieves some combination of raw data, portals, and OS handles from a
  // portal, as placed by a prior put operation on the opposite portal.
  //
  // On input, the values pointed to by `num_bytes`, `num_portals`, and
  // `num_os_handles` must specify the capacity of each corresponding buffer
  // argument. A null pointer is equivalent to a pointer pointing to a zero
  // value. It is an error to specify a non-zero capacity if the corresponding
  // buffer (`data`, `portals`, or `os_handles`, respectively) is null.
  //
  // Normally the data consumed by this call is copied directly to the address
  // given by the `data` argument, and `*num_bytes` specifies how many bytes of
  // storage are available there.  If an application wishes to read directly
  // from portal memory instead, a two-phase get operation can be used by
  // calling BeginGet() and EndGet() as defined below.
  //
  // `flags` is ignored and must be IPCZ_NO_FLAGS.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if there was at a parcel available in the portal's queue
  //        and its data, portals, and OS handles were able to be copied into
  //        the caller's provided buffers. In this case values pointed to by
  //        `num_bytes`, `num_portals`, and `num_os_handles` (for each one that
  //        is non-null) are updated to reflect what was actually consumed. Note
  //        that the caller assumes ownership of all returned portals and OS
  //        handles.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `data` is null but
  //        `*num_bytes` is non-zero, `portals` is null but `*num_portals` is
  //        non-zero, or `os_handles` is null but `*num_os_handles` is non-zero.
  //
  //    IPCZ_RESULT_RESOURCE_EXHAUSTED if the next available parcel would exceed
  //        the caller's specified capacity for either data bytes, portals, or
  //        OS handles. In this case, any non-null size pointer is updated to
  //        convey the minimum capacity that would have been required for an
  //        otherwise identical Get() call to have succeeded. Callers observing
  //        this result may wish to allocate storage accordingly and retry with
  //        updated parameters.
  //
  //    IPCZ_RESULT_UNAVAILABLE if the portal's parcel queue is currently empty.
  //        In this case callers should wait before attempting to get anything
  //        from the same portal again.
  //
  //    IPCZ_RESULT_NOT_FOUND if there are no more parcels in the portal's queue
  //        AND the opposite portal is known to be closed. If this result is
  //        returned, no parcels can ever be read from this portal again.
  //
  //    IPCZ_RESULT_ALREADY_EXISTS if there is a two-phase get operation in
  //        progress on `portal`.
  IpczResult (*Get)(IpczHandle portal,
                    uint32_t flags,
                    const void* options,
                    void* data,
                    uint32_t* num_bytes,
                    IpczHandle* portals,
                    uint32_t* num_portals,
                    struct IpczOSHandle* os_handles,
                    uint32_t* num_os_handles);

  // Begins a two-phase get operation on `portal` to retreive data, portals, and
  // OS handles. While a two-phase get operation is in progress on a portal, all
  // other get operations on the same portal will fail with
  // IPCZ_RESULT_ALREADY_EXISTS.
  //
  // Unlike a plain Get() call, two-phase get operations allow the application
  // to read directly from parcel memory, potentially reducing memory access
  // costs by eliminating redundant copying and caching.
  //
  // If `data` or `num_bytes` is null and the available parcel has at least one
  // byte of data, this returns IPCZ_RESULT_RESOURCE_EXHAUSTED.
  //
  // Otherwise if `data` and `num_bytes` are non-null, a successful BeginGet()
  // updates them to expose parcel memory for the application to consume.
  //
  // If `num_portals` or `num_os_handles` is non-null and this call is
  // successful, the value(s) pointed to will respectively reflect the number of
  // portals and OS handles in the next available parcel. The portals and
  // handles are not retreived from the portal until the application issues a
  // corresponding call to EndGet().
  //
  // NOTE: When performing two-phase get operations, callers should be mindful
  // of time-of-check/time-of-use (TOCTOU) vulnerabilities. Exposed parcel
  // memory may be shared with (and writable in) the process which placed the
  // parcel into the portal, and that process may not be trustworthy. In such
  // cases, applications should be careful to copy the data out before
  // validating and using it.
  //
  // `flags` is ignored and must be IPCZ_NO_FLAGS.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the two-phase get was successfully initiated. In this
  //        case both `*data` and `*num_bytes` are updated (if `data` and
  //        `num_bytes` were non-null) to describe the portal memory from which
  //        the application is free to read parcel data. If `num_portals` or
  //        `num_os_handles` is non-null, the value(s) pointed to are updated to
  //        reflect the count of available portals and OS handles.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid.
  //
  //    IPCZ_RESULT_RESOURCE_EXHAUSTED if the next available parcel has at least
  //        one data byte but `data` or `num_bytes` is null.
  //
  //    IPCZ_RESULT_UNAVAILABLE if the portal's parcel queue is currently empty.
  //        In this case callers should wait before attempting to get anything
  //        from the same portal again.
  //
  //    IPCZ_RESULT_NOT_FOUND if there are no more parcels in the portal's queue
  //        AND the opposite portal is known to be closed. In this case, no get
  //        operation can ever succeed again on this portal.
  //
  //    IPCZ_RESULT_ALREADY_EXISTS if there is already a two-phase get operation
  //        in progress on `portal`.
  IpczResult (*BeginGet)(IpczHandle portal,
                         uint32_t flags,
                         const void* options,
                         const void** data,
                         uint32_t* num_bytes,
                         uint32_t* num_portals,
                         uint32_t* num_os_handles);

  // Ends the two-phase get operation started by the most recent successful call
  // to BeginGet() on `portal`.
  //
  // `num_bytes_consumed` specifies the number of bytes actually read from the
  // buffer that was returned from the original BeginGet() call.
  //
  // `portals`, `num_portals`, `os_handles`, and `num_os_handles` are used
  // and behave exactly the same as with a Get() call. Note that BeginGet()
  // also exposes the number of available portals and OS handles as its outputs,
  // so applications expecting to receive either during a two-phase get should
  // read those outputs and use them as hints to avoid a redundant call to
  // EndGet() resulting in IPCZ_RESOURCE_EXHAUSTED.
  //
  // If IPCZ_END_GET_ABORT is given in `flags` and there is a two-phase get
  // operation in progress on `portal`, all other arguments are ignored and the
  // pending operation is cancelled without consuming any data from the portal.
  // Note that any portals or OS handles which were already consumed by the
  // corresponding BeginGet() remain property of the caller.
  //
  // `options` is unused and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the two-phase operation was successfully completed or
  //        aborted. Note that if the frontmost parcel wasn't fully consumed by
  //        the caller, it will remain in queue with the rest of its data intact
  //        for a subsequent get operation to retrieve.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid or
  //        `num_bytes_consumed` is larger than the capacity of the buffer
  //        originally returned by BeginGet().
  //
  //    IPCZ_RESULT_RESOURCE_EXHAUSTED if the next available parcel would exceed
  //        the caller's specified capacity portals or OS handles. In this case,
  //        any non-null size pointer is updated to convey the minimum capacity
  //        that would have been required for an otherwise identical EndGet()
  //        call to have succeeded. Callers observing this result may wish to
  //        allocate storage accordingly and retry with updated parameters.
  //
  //    IPCZ_RESULT_FAILED_PRECONDITION if there was no two-phase get operation
  //        in progress on `portal`.
  IpczResult (*EndGet)(IpczHandle portal,
                       uint32_t num_bytes_consumed,
                       IpczEndGetFlags flags,
                       const void* options,
                       IpczHandle* portals,
                       uint32_t* num_portals,
                       struct IpczOSHandle* os_handles,
                       uint32_t* num_os_handles);

  // Creates a trap to catch interesting changes to a portal's state.
  //
  // Traps are created in a disarmed state and must be armed explicitly by a
  // call to ArmTrap(). Once armed they can fire the given `handler` exactly
  // once, at which point they are automatically disarmed again.
  //
  // An armed trap will invoke its `handler` as soon as any condition described
  // by `conditions` becomes satisfied. For example if `conditions` specifies an
  // interest in IPCZ_TRAP_CONDITION_LOCAL_PARCELS with a value of 1 in
  // `min_local_parcels` and the trap is armed, then `handler` will be invoked
  // as soon as there is at least one incoming parcel available for retrieval on
  // `portal`.
  //
  // When `handler` is invoked, it's passed the value of `context`, which
  // applications may use to differentiate between multiple traps using the same
  // handler. It's also passed an IpczPortalStatus structure indicating details
  // about the portal's state at the time of the invocation.
  //
  // A disarmed trap will never invoke its handler.
  //
  // Note that a portal's state may be changed by any thread, including an
  // internal ipcz thread observing incoming parcels from out-of-process.
  // Because of this, application developers must be mindful of thread safety
  // within `handler` and whatever logic might arm or destroy traps.
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the trap was created successfully as described above.
  //        `*trap* is populated with a handle the application may use in
  //        subsequent calls to ArmTrap() or DestroyTrap().
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `conditions` is
  //        null or invalid, `handler` is null, or `trap` is null.
  IpczResult (*CreateTrap)(IpczHandle portal,
                           const struct IpczTrapConditions* conditions,
                           IpczTrapEventHandler handler,
                           uint64_t context,
                           uint32_t flags,
                           const void* options,
                           IpczHandle* trap);

  // Arms a trap.
  //
  // If successful the trap may invoke its event handler at any time on any
  // thread, as soon as its specified conditions become satisfied.
  // Once this occurs the trap is once again disarmed and must be re-armed by
  // another call to ArmTrap().
  //
  // If the trap's conditions are already satisfied at the time of this call,
  // the call fails with IPCZ_RESULT_FAILED_PRECONDITION. See below for
  // additional details.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the trap was successfully armed. In this case the
  //        `conditions` and `status` arguments ignored.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `trap` is invalid,
  //        `satisfied_condition_flags` is non-null but invalid, or `status` is
  //        non-null but invalid.
  //
  //    IPCZ_RESULT_ALREADY_EXISTS if the trap was already armed. In this case
  //        the `conditions` and `status` arguments ignored.
  //
  //    IPCZ_RESULT_FAILED_PRECONDITION if one or more of the trap's conditions
  //        are already satisfied, such that the trap would fire an event
  //        immediately once armed. If `satisfied_condition_flags` is non-null
  //        it will be populated to indicate which satisfied condition(s)
  //        blocked the arming of the trap, and if `status` is not null it will
  //        be populated with details about the portal's current status.
  IpczResult (*ArmTrap)(IpczHandle trap,
                        uint32_t flags,
                        const void* options,
                        IpczTrapConditionFlags* satisfied_condition_flags,
                        struct IpczPortalStatus* status);

  // Destroys a trap.
  //
  // Upon success the specified trap will no longer exist and it is guaranteed
  // to never invoke its handler again.
  //
  // Note that an event may occur on another thread which trips the trap and
  // invokes its handler immediately before or during this call, so applications
  // must take care to synchronize access to any state shared between the trap
  // handler and whatever logic manages the trap's lifecycle.
  //
  // If IPCZ_DESTROY_TRAP_BLOCKING is specified, DestroyTrap() blocks until any
  // currently running event handler for this trap is finished executing on any
  // thread. As a consequence if this flag is given to a call made within the
  // destroyed trap's own event handler, the call is guaranteed to deadlock.
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the trap was successfully destroyed. The trap's
  //        handler will never be invoked after this result is returned.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `trap` is invalid.
  IpczResult (*DestroyTrap)(IpczHandle trap,
                            IpczDestroyTrapFlags flags,
                            const void* options);
};

// Populates `api` with a table of ipcz API functions. The `size` field must be
// set by the caller to the size of the structure before issuing this call.
//
// If the caller is linking statically against the ipcz implementation, they can
// reasonably expect a complete filled-in API structure. If however the
// application is linked against ipcz dynamically, it's possible that the
// available implementation will be older or newer than the application's own
// copy of the ipcz API definitions.
//
// In any case, upon return `api->size` will indicate the size of the function
// table actually populated and therefore which version of the ipcz
// implementation is in use. Note that this size will never exceed the input
// value of `api->size`: if the caller is built against an older version than
// what is available, the available implementation will only populate the
// functions appropriate for that older version.
//
// Returns:
//
//    IPCZ_RESULT_OK if `api` was successfully populated. In this case
//       `api->size` effectively indicates the API version provided, and the
//       appopriate function pointers within `api` are filled in.
//
//    IPCZ_RESULT_INVALID_ARGUMENT if `api` was invalid, for example if the
//       caller's provided `api->size` is less than the size of the function
//       table required to host API version 0.
IpczResult IpczGetAPI(struct IpczAPI* api);

}  // extern "C"

#endif  // IPCZ_INCLUDE_IPCZ_IPCZ_H_
