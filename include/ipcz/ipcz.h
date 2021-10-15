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

// Enumeration to specify which type of platform-specific OS-level transport
// object is identified by an IpczOSTransport structure. OS transports are used
// with OpenRemotePortal() and AcceptRemotePortal() to bootstrap ipcz
// communication between two nodes. See documentation on those functions.
typedef uint32_t IpczOSTransportType;

// A UNIX domain socket. This is comprised of a single OS handle of type
// IPCZ_OS_HANDLE_FILE_DESCRIPTOR, whose value is a file descriptor identifying
// the socket to use.
#define IPCZ_OS_TRANSPORT_UNIX_SOCKET ((IpczOSTransportType)0)

// A Windows I/O object such as a named pipe, which supports WriteFile and
// ReadFile APIs. This is comprised of a single OS handle of type
// IPCZ_OS_HANDLE_WINDOWS, whose value is a HANDLE identifying the I/O object to
// use.
#define IPCZ_OS_TRANSPORT_WINDOWS_IO ((IpczOSTransportType)1)

// A Fuchsia channel. This is comprised of a single OS handle of type
// IPCZ_OS_HANDLE_FUCHSIA, whose value is a zx_handle_t identifying the Fuchsia
// channel endpoint to use.
#define IPCZ_OS_TRANSPORT_FUCHSIA_CHANNEL ((IpczOSTransportType)2)

// Describes an OS IPC transport which can be used to bootstrap a remote portal
// between two processes. See OpenRemotePortal() and AcceptRemotePortal().
struct IPCZ_ALIGN(8) IpczOSTransport {
  // The exact size of this structure in bytes. Must be set accurately in any
  // instance of this structure before passing it to an ipcz function.
  uint32_t size;

  // The type of transport described by this structure. See the definitions
  // above. Not all types are supported on all platforms.
  IpczOSTransportType type;

  // An array of handles to any OS resources comprising this transport. See each
  // type description for details regarding what kind of resource(s) must be
  // given for each type.
  const struct IpczOSHandle* handles;
  uint32_t num_handles;
};

// See CreateNode() and the IPCZ_CREATE_NODE_* flag descriptions below.
typedef uint32_t IpczCreateNodeFlags;

// Indicates that the created node will serve as a broker.
//
// Brokers are expected to live in relatively trusted and privileged processes,
// as they're responsible for helping other nodes establish direct lines of
// communication, as well as in some cases facilitating proxying of data and
// relaying of OS handles.
//
// Broker nodes do not expose any additional APIs or require much other special
// care on the part of the application**, but for most applications on most
// platforms, it's beneficial for a cluster of nodes to always have at least one
// node designated as a broker. Without at least one broker it may not be
// possible to transfer various types of handles through portals spanning a
// process boundary.
//
// ** See notes on DestroyNode() regarding destruction of broker nodes.
#define IPCZ_CREATE_NODE_AS_BROKER IPCZ_FLAG_BIT(0)

// Optional limits provided by IpczPutOptions for Put() or IpczBeginPutOptions
// for BeginPut().
struct IPCZ_ALIGN(8) IpczPutLimits {
  // The exact size of this structure in bytes. Must be set accurately before
  // passing the structure to any API functions.
  uint32_t size;

  // If non-zero, this specifies the maximum number of parcels to allow in the
  // portal's queue. If a Put() or BeginPut() call specifying this limit would
  // cause the number of queued parcels to exceed this value, it will fail with
  // IPCZ_RESULT_RESOURCE_EXHAUSTED.
  uint32_t max_queued_parcels;

  // If non-zero, this specifies the maxmimum number of data bytes to allow in
  // the portal's queue. If a Put() or BeginPut() call specifying this limit
  // would cause the number of queued data bytes across all queued parcels to
  // exceed this value, it will fail with IPCZ_RESULT_RESOURCE_EXHAUSTED.
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
// requested by their `*num_data_bytes` argument to BeginPut(). If the
// implementation would prefer a smaller chunk of data or if the requested size
// would exceed limits specified in the call's corresponding IpczPutLimits,
// passing this flag may allow the call to succeed while returning a smaller
// acceptable value in `*num_data_bytes`, rather than simply failing the call
// with IPCZ_RESULT_RESOURCE_EXHAUSTED.
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
// aborted without committing any data or handles to the portal.
#define IPCZ_END_PUT_ABORT IPCZ_FLAG_BIT(0)

// See EndGet() and the IPC_END_GET_* flag descriptions below.
typedef uint32_t IpczEndGetFlags;

// If this flag is given to EndGet(), any in-progress two-phase get operation is
// aborted without consuming any data from the portal.
#define IPCZ_END_GET_ABORT IPCZ_FLAG_BIT(0)

// Status field selectors given to QueryPortalStatus() or CreateMonitor() to
// select which fields of an IpczPortalStatus are interesting to the caller.
//
// When an IpczPortalStatus is populated by ipcz on behalf of a caller, only the
// fields selected by these flags are populated, and all other fields are left
// with unspecified values. See IPCZ_PORTAL_STATUS_FIELD_* flags described
// below.
typedef uint32_t IpczPortalStatusFieldFlags;

// Indicates an interest in the portal's status bits. See IpczPortalStatus
// and IPCZ_PORTAL_STATUS_BIT_* flags described below.
#define IPCZ_PORTAL_STATUS_FIELD_BITS IPCZ_FLAG_BIT(0)

// Indicates an interest in the number of queued parcels yet to be retrieved
// from this portal. See IpczPortalStatus.
#define IPCZ_PORTAL_STATUS_FIELD_LOCAL_PARCELS IPCZ_FLAG_BIT(1)

// Indicates an interest in the number of queued bytes (across all queued
// parcels) yet to be retrieved from this portal. See IpczPortalStatus.
#define IPCZ_PORTAL_STATUS_FIELD_LOCAL_BYTES IPCZ_FLAG_BIT(2)

// Indicates an interest in the number of queued parcels yet to be retrieved by
// the opposite portal. See IpczPortalStatus.
#define IPCZ_PORTAL_STATUS_FIELD_REMOTE_PARCELS IPCZ_FLAG_BIT(3)

// Indicates an interest in the number of queued bytes (across all queued
// parcels) yet to be retrieved by the opposite portal. See IpczPortalStatus.
#define IPCZ_PORTAL_STATUS_FIELD_REMOTE_BYTES IPCZ_FLAG_BIT(4)

// Flags given by the `status_bits` field in IpczPortalStatus.
typedef uint32_t IpczPortalStatusBits;

// Indicates that the opposite portal is closed. Subsequent put operations on
// this portal will always fail with IPCZ_RESULT_NOT_FOUND. If there are not
// currently any unretrieved parcels in the portal either, subsequent get
// operations will also fail with the same error.
#define IPCZ_PORTAL_STATUS_BIT_CLOSED IPCZ_FLAG_BIT(0)

// Information returned by QueryPortalStatus() or provided to
// IpczMonitorEventHandlers when a portal monitor's conditions on their portal.
struct IPCZ_ALIGN(8) IpczPortalStatus {
  // The exact size of this structure in bytes. Must be set accurately before
  // passing the structure to any functions.
  uint32_t size;

  // Flags. Only populated if IPCZ_PORTAL_STATUS_FIELD_BITS is given. See the
  // IPCZ_PORTAL_STATUS_BIT_* flags described above for possible bits set in
  // this value.
  IpczPortalStatusBits bits;

  // The number of unretrieved parcels queued on this portal.
  //
  // Set only if IPCZ_PORTAL_STATUS_FIELD_LOCAL_PARCELS is selected on the
  // corresponding CreateMonitor() or QueryPortalStatus() call. Otherwise this
  // value is unspecified.
  uint64_t num_local_parcels;

  // The number of unretrieved bytes (across all unretrieved parcels) queued on
  // this portal.
  //
  // Set only if IPCZ_PORTAL_STATUS_FIELD_LOCAL_BYTES is selected on the
  // corresponding CreateMonitor() or QueryPortalStatus() call. Otherwise this
  // value is unspecified.
  uint64_t num_local_bytes;

  // The number of unretrieved parcels queued on the opposite portal.
  //
  // Set only if IPCZ_PORTAL_STATUS_FIELD_REMOTE_PARCELS is selected on the
  // corresponding CreateMonitor() or QueryPortalStatus() call. Otherwise this
  // value is unspecified.
  uint64_t num_remote_parcels;

  // The number of unretrieved bytes (across all unretrieved parcels) queued on
  // the opposite portal.
  //
  // Set only if IPCZ_PORTAL_STATUS_FIELD_REMOTE_BYTES is selected on the
  // corresponding CreateMonitor() or QueryPortalStatus() call. Otherwise this
  // value is unspecified.
  uint64_t num_remote_bytes;
};

// Flags given to IpczMonitorConditions to indicate which types of conditions a
// monitor should observe.
typedef uint32_t IpczMonitorConditionFlags;

// Triggers a monitor event whenever the opposite portal is closed. Typically
// applications are interested in the more specific IPCZ_MONITOR_CONDITION_DEAD.
#define IPCZ_MONITOR_CONDITION_CLOSED IPCZ_FLAG_BIT(0)

// Triggers a monitor event whenever there are no more parcels available to
// retrieve from this portal AND the opposite portal is closed. This means the
// portal will never again have parcels to retrieve and is effectively useless
#define IPCZ_MONITOR_CONDITION_DEAD IPCZ_FLAG_BIT(1)

// Triggers a monitor event whenever the number of parcels queued for retrieval
// by this portal exceeds the threshold given by `min_local_parcels` in
// IpczMonitorConditions.
#define IPCZ_MONITOR_CONDITION_LOCAL_PARCELS IPCZ_FLAG_BIT(2)

// Triggers a monitor event whenever the number of bytes queued for retrieval by
// this portal exceeds the threshold given by `min_local_bytes` in
// IpczMonitorConditions.
#define IPCZ_MONITOR_CONDITION_LOCAL_BYTES IPCZ_FLAG_BIT(3)

// Triggers a monitor event whenever the number of parcels queued for retrieval
// on the opposite portal drops below the threshold given by
// `max_remote_parcels` in IpczMonitorConditions.
#define IPCZ_MONITOR_CONDITION_REMOTE_PARCELS IPCZ_FLAG_BIT(2)

// Triggers a monitor event whenever the number of bytes queued for retrieval
// on the opposite portal drops below the threshold given by `max_remote_bytes`
// in IpczMonitorConditions.
#define IPCZ_MONITOR_CONDITION_LOCAL_BYTES IPCZ_FLAG_BIT(3)

// A structure describing portal conditions which should trigger an active
// monitor to invoke its event handler.
struct IPCZ_ALIGN(8) IpczMonitorConditions {
  // The exact size of this structure in bytes. Must be set accurately before
  // passing the structure to CreateMonitor() via IpczMonitorDescriptor.
  uint32_t size;

  // See the IPCZ_MONITOR_CONDITION_* flags described above.
  IpczMonitorConditionFlags flags;

  // See IPCZ_MONITOR_CONDITION_LOCAL_PARCELS. If that flag is not set in
  // `flags`, this field is ignored.
  uint32_t min_local_parcels;

  // See IPCZ_MONITOR_CONDITION_LOCAL_BYTES. If that flag is not set in
  // `flags`, this field is ignored.
  uint32_t min_local_bytes;

  // See IPCZ_MONITOR_CONDITION_REMOTE_PARCELS. If that flag is not set in
  // `flags`, this field is ignored.
  uint32_t max_remote_parcels;

  // See IPCZ_MONITOR_CONDITION_REMOTE_BYTES. If that flag is not set in
  // `flags`, this field is ignored.
  uint32_t max_remote_bytes;
};

// Structure passed to each IpczMonitorEventHandler invocation with details
// about the event.
struct IPCZ_ALIGN(8) IpczMonitorEvent {
  // The context value originally given to CreateMonitor() when creating the
  // monitor which triggered this event.
  uintptr_t context;

  // Flags indicating which condition(s) triggered this event.
  IpczMonitorConditionFlags condition_flags;

  // The current status of the portal which triggered this event. Only the
  // fields indicated by the monitor's IpczPortalStatusFields value will be
  // populated. The rest are unspecified.
  const IpczPortalStatus* portal_status;
};

// An application-defined function to be invoked by a portal monitor when its
// observed conditions are satisfied on the monitored portal.
typedef void (*IpczMonitorEventHandler)(const struct IpczMonitorEvent* event);

// A structure describing a new portal monitor to create with CreateMonitor().
struct IPCZ_ALIGN(8) IpczMonitorDescriptor {
  // The exact size of this structure in bytes. Must be set accurately before
  // passing the structure to CreateMonitor().
  uint32_t size;

  // The conditions under which this monitor should invoke its handler when
  // active.
  const struct IpczMonitorConditions* conditions;

  // The event handler to invoke any time this monitor is active and its
  // conditions become satisfied on the monitored portal.
  IpczMonitorEventHandler handler;

  // An arbitrary application-provided value which will be passed to every
  // invocation of `handler` made on behalf of this monitor. This allows a
  // common event handler to differentiate between the monitors invoking it.
  uintptr_t context;

  // Flags selecting which IpczPortalStatus fields are interesting to `handler`.
  // Every invocation of `handler` is provided an instance of IpczPortalStatus,
  // but only the fields specified by this value will be populated with each
  // invocation. Other fields will have unspecified values.
  IpczPortalStatusFieldFlags status_fields;
};

extern "C" {

// Table of API functions defined by ipcz. Instances of this structure may be
// populated by passing them to IpczGetAPI().
//
// Note that all functions follow a consistent parameter ordering:
//
//   1. Node (if applicable)
//   2. Function-specific input values
//   3. Function-specific in/out values
//   4. Flags (possibly untyped and unused)
//   5. Options (possibly untyped and unused)
//   6. Output values
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
  // If `flags` contains IPCZ_CREATE_NODE_AS_BROKER then the node will act as a
  // broker in its connected cluster of nodes. See details on that flag
  // description above.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if a new node was created. In this case, `*node` is
  //        populated with a valid node handle upon return.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `node` is null.
  IpczResult (*CreateNode)(IpczCreateNodeFlags flags,
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
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // NOTE: If `node` is the only broker node in its cluster of connected nodes,
  // certain operations across the cluster -- such as handle transmission
  // through portals -- may begin to fail spontaneously on some platforms once
  // destruction is complete.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if `node` was destroyed.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `node` is invalid.
  IpczResult (*DestroyNode)(IpczHandle node,
                            uint32_t flags,
                            const void* options);

  // Opens two new portals which exist as each other's opposite.
  //
  // Data and handles can be put in a portal with put operations (see Put(),
  // BeginPut(), EndPut()). Anything placed into a portal can be retrieved in
  // the same order by get operations (Get(), BeginGet(), EndGet()) on the
  // opposite portal. These operations are bidirectional: both portals are equal
  // peers which support the same set of operations.
  //
  // Portals may themselves be placed into other portals on the same node and
  // then retrieved from the opposite portal, potentially on a different node.
  //
  // To open a pair of portals which span two different nodes at creation time,
  // use OpenRemotePortal() instead.
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

  // Opens a new local portal which will be linked to an opposite portal in
  // another node. The local portal is usable immediately, and the opposite
  // portal is established asynchronously by another node making a corresponding
  // call to AcceptRemotePortal().
  //
  // The application is responsible for establishing an appropriate
  // platform-specific transport mechanism between this node and whatever node
  // will accept the new portal. For example, on Linux an application may create
  // a new UNIX domain socket pair via socketpair(), passing one end in this
  // call's `transport` argument, while getting the other end to some other
  // process in the system (for example, to a child process via fork()) which
  // may then use that end of the socket pair with a corresponding
  // AcceptRemotePortal() call on their own node.
  //
  // This function, in combination with AcceptRemotePortal(), is how new nodes
  // are able to join an existing cluster of nodes. Due to the relative
  // complexity and overhead of this setup though, most remote portals should
  // not be created using these functions: instead these functions should be
  // used only when necessary to bootstrap communication. Once a single remote
  // portal is established, local portals can be efficiently transferred through
  // it to become remote portals themselves.
  //
  // Note that `target_process` MAY be null on some platforms without causing
  // any issues, but cross-platform should always provide it for maximum
  // compatibility. This operation will always fail on Windows with
  // IPCZ_RESULT_INVALID_ARGUMENT if `target_process` is null.
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if portal creation was successful. `portal` is populated
  //        with a handle to the new local portal on `node`. Note that creation
  //        of the opposite portal may still be pending, and it will complete
  //        only once some other node uses AcceptRemotePortal() on an OS
  //        transport linked to the one given to this call. A successful result
  //        therefore does not guarantee that the opposite portal will be fully
  //        established. If a failure does occur before that happens, including
  //        destruction or dysfunction of the underlying OS transport used, the
  //        local portal will eventually behave as if its opposite portal has
  //        been closed.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `node` is invalid, `portal` is null, or
  //        `transport` does not specify a valid OS transport. This may also be
  //        returned if `target_process` is null on platforms which require a
  //        valid process handle.
  //
  //    IPCZ_RESULT_PERMISSION_DENIED if some OS-specific operation failed to
  //        use `transport` as expected prior to this call returning, for some
  //        reason directly related to user or application privileges within the
  //        system (e.g. failure to duplicate an OS handle and write to an I/O
  //        object).
  //
  //    IPCZ_RESULT_FAILED_UNKNOWN if some OS-specific operation failed to use
  //        `transport` as expected prior to this call returning, for some
  //        reason unrelated to user or application privileges within the
  //        system.
  IpczResult (*OpenRemotePortal)(
      IpczHandle node,
      const struct IpczOSTransport* transport,
      const struct IpczOSProcessHandle* target_process,
      uint32_t flags,
      const void* options,
      IpczHandle* portal);

  // Accepts a new portal from a remote source. Upon success this returns a
  // local portal handle which can be used immediately by the calling node. The
  // opposite portal may already be established or will be established
  // asynchronously by a remote node calling OpenRemotePortal() with an OS
  // transport related to the `transport` given here (for example, the two
  // transports may be either side of a UNIX domain socket pair.)
  //
  // See OpenRemotePortal() for additional details.
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if local portal creation was successful, and asynchronous
  //        establishment of the opposite portal was initiated. In this case
  //        `*portal` is populated with a handle to the new local portal. See
  //        additional notes on the IPCZ_RESULT_OK result of OpenRemotePortal().
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `node` is invalid, `portal` is null, or
  //        `transport` does not specify a valid OS transport.
  //
  //    IPCZ_RESULT_PERMISSION_DENIED if some OS-specific operation failed to
  //        use `transport` as expected prior to this call returning, for some
  //        reason directly related to user or application privileges within the
  //        system (e.g. failure to duplicate an OS handle and write to an I/O
  //        object).
  //
  //    IPCZ_RESULT_FAILED_UNKNOWN if some OS-specific operation failed to use
  //        `transport` as expected prior to this call returning, for some
  //        reason unrelated to user or application privileges within the
  //        system.
  IpczResult (*AcceptRemotePortal)(IpczHandle node,
                                   const struct IpczOSTransport* transport,
                                   uint32_t flags,
                                   const void* options,
                                   IpczHandle* portal);

  // Closes the portal identified by `portal`.
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
  //    IPCZ_RESULT_OK if the requested query was completed successfully. Any
  //        fields indicated by `field_flags` will have a corresponding field
  //        populated in `status` upon return. Other fields are left in an
  //        unspecified state.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT `portal` is invalid. `status` is null, or
  //        `status` is non-null but invalid.
  IpczResult (*QueryPortalStatus)(IpczHandle portal,
                                  IpczPortalStatusFieldFlags field_flags,
                                  uint32_t flags,
                                  const void* options,
                                  struct IpczPortalStatus* status);

  // Puts any combination of raw data, ipcz handles, and OS handles into the
  // portal identified by `portal`. Everything put into a portal can be
  // retrieved in the same order by a corresponding get operation on the
  // opposite portal. (See OpenPortals() and OpenRemotePortal() for the meaning
  // of "opposite" in this context.)
  //
  // `flags` is unused and must be IPCZ_NO_FLAGS.
  //
  // `options` may be null.
  //
  // If this call fails (returning anything other than IPCZ_RESULT_OK), any
  // provided ipcz or OS handles remain property of the caller. If it succeeds,
  // their ownership is assumed by ipcz.
  //
  // Data to be submitted is read directly from the address given by the `data`
  // argument, and `num_data_bytes` specifies how many bytes of data to copy
  // from there.
  //
  // Callers may wish to request a view directly into portal memory for direct
  // writing (for example, in cases where copying first from some other source
  // into a separate application buffer just for Put() would be redundant.) In
  // such cases, a two-phase put operation can instead be used by calling
  // BeginPut() and EndPut() as defined below.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the provided data and handles were successfully placed
  //        into the portal as a new parcel.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `data` is null but
  //        `num_data_bytes` is non-zero, `ipcz_handles` is null but
  //        `num_ipcz_handles` is non-zero, `os_handles` is null but
  //        `num_os_handles` is non-zero, `options` is non-null but invalid, or
  //        one of the handles in `ipcz_handles` is equal to `portal` or its
  //        local opposite (if applicable).
  //
  //    IPCZ_RESULT_RESOURCE_EXHAUSTED if `options->limits` is non-null and at
  //        least one of the specified limits would be violated by the
  //        successful completion of this call.
  //
  //    IPCZ_RESULT_ALREADY_EXISTS there is a two-phase put operation in
  //        progress on `portal`, meaning BeginPut() has been called without a
  //        corresponding EndPut().
  //
  //    IPCZ_RESULT_NOT_FOUND if it is known that the opposite portal has
  //        already been closed and anything put into this portal would be lost.
  //
  //    IPCZ_RESULT_PERMISSION_DENIED if the caller attempted to place handles
  //        into the portal which could not be transferred to the other side due
  //        to OS-level privilege constraints.
  IpczResult (*Put)(IpczHandle portal,
                    const void* data,
                    uint32_t num_data_bytes,
                    const IpczHandle* ipcz_handles,
                    uint32_t num_ipcz_handles,
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
  // The input value of `*num_data_bytes` tells ipcz how much data the caller
  // would like to place into the portal.
  //
  // Limits provided to BeginPut() elicit similar behavior to Put(), with the
  // exception that `flags` may specify IPCZ_BEGIN_PUT_ALLOW_PARTIAL to allow
  // BeginPut() to succeed even the caller's suggested value of
  // `*num_data_bytes` would cause the portal to exceed the maximum queued byte
  // limit given by `options->limits`. In that case BeginPut() may update
  // `*num_data_bytes` to reflect the remaining capacity of the portal, allowing
  // the caller to commit at least some portion of their data with EndPut().
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
  //        may copy its data, and `*num_data_bytes` is updated to reflect the
  //        capacity of that buffer, which may be greater than (or less than, if
  //        and only if IPCZ_BEGIN_PUT_ALLOW_PARTIAL was set in `flags`) the
  //        capacity requested by the input value of `*num_data_bytes`.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `*num_data_bytes`
  //        is non-zero but `data` is null, or options is non-null and invalid.
  //
  //    IPCZ_RESULT_RESOURCE_EXHAUSTED if completing the put with the number of
  //        bytes specified by `*num_data_bytes` would cause the portal to
  //        exceed the queued parcel limit or (if IPCZ_BEGIN_PUT_ALLOW_PARTIAL
  //        is not specified in `flags`) data byte limit specified by
  //        `options->limits`.
  //
  //    IPCZ_RESULT_ALREADY_EXISTS if there is already a two-phase put operation
  //        in progress on `portal`.
  //
  //    IPCZ_RESULT_NOT_FOUND if it is known that the opposite portal has
  //        already been closed and anything put into this portal would be lost.
  IpczResult (*BeginPut)(IpczHandle portal,
                         uint32_t* num_data_bytes,
                         IpczBeginPutFlags flags,
                         const struct IpczBeginPutOptions* options,
                         void** data);

  // Ends the two-phase put operation started by the most recent successful call
  // to BeginPut() on `portal`.
  //
  // `num_data_bytes_produced` specifies the number of bytes actually written
  // into the buffer that was returned from the original BeginPut() call.
  //
  // Usage of `ipcz_handles`, `num_ipcz_handles`, `os_handles`, and
  // `num_os_handles` is identical to Put().
  //
  // If this call fails (returning anything other than IPCZ_RESULT_OK), any
  // provided ipcz or OS handles remain property of the caller. If it succeeds,
  // their ownership is assumed by ipcz.
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
  //        aborted. If not aborted, all data and handles were committed to a
  //        new parcel enqueued for retrieval by the opposite portal.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `num_ipcz_handles`
  //        is non-zero but `ipcz_handles` is null, `num_os_handles` is non-zero
  //        but `os_handles` is null, or `num_data_bytes_produced` is larger
  //        than the capacity of the buffer originally returned by BeginPut().
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
                       uint32_t num_data_bytes_produced,
                       const IpczHandle* ipcz_handles,
                       uint32_t num_ipcz_handles,
                       const IpczOSHandle* os_handles,
                       uint32_t num_os_handles,
                       IpczEndPutFlags flags,
                       const void* options);

  // Retrieves some combination of raw data, ipcz handles, and OS handles from
  // a portal, as placed by a prior put operation on the opposite portal.
  //
  // On input, the values pointed to by `num_data_bytes`, `num_ipcz_handles`,
  // and `num_os_handles` must specify the capacity of each corresponding buffer
  // argument. A null pointer is equivalent to a pointer pointing to a zero
  // value. It is an error to specify a non-zero capacity if the corresponding
  // buffer (`data`, `ipcz_handles`, or `os_handles`, respectively) is null.
  //
  // Normally the data consumed by this call is copied directly to the address
  // given by the `data` argument, and `*num_data_bytes` specifies how many
  // bytes of storage are available there.  If an application wishes to read
  // directly from portal memory instead, a two-phase get operation can be used
  // by calling BeginGet() and EndGet() as defined below.
  //
  // `flags` is ignored and must be IPCZ_NO_FLAGS.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if there was at a parcel available in the portal's queue
  //        and its data and handles were able to be copied into the caller's
  //        provided buffers. In this case values pointed to by
  //        `num_data_bytes`, `num_ipcz_handles`, and `num_os_handles` (for each
  //        one that is non-null) are updated to reflect what was actually
  //        consumed. Note that the caller assumes ownership of all returned
  //        ipcz and OS handles.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `data` is null but
  //        `*num_data_bytes` is non-zero, `ipcz_handles` is null but
  //        `*num_ipcz_handles` is non-zero, or `os_handles` is null but
  //        `*num_is_handles` is non-zero.
  //
  //    IPCZ_RESULT_RESOURCE_EXHAUSTED if the next available parcel would exceed
  //        the caller's specified capacity for either data bytes, ipcz handles,
  //        or OS handles. In this case, any non-null size pointer is updated to
  //        convey the minimum capacity that would have been required for an
  //        otherwise identical IpczGet call to have succeeded. Callers
  //        observing this result may wish to allocate storage accordingly and
  //        retry with updated parameters.
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
                    void* data,
                    uint32_t* num_data_bytes,
                    IpczHandle* ipcz_handles,
                    uint32_t* num_ipcz_handles,
                    struct IpczOSHandle* os_handles,
                    uint32_t* num_os_handles,
                    uint32_t flags,
                    const void* options);

  // Begins a two-phase get operation on `portal` to retreive data and/or
  // handles from the frontmost parcel in its incoming queue. While a two-phase
  // get operation is in progress on a portal, all other get operations on the
  // portal will fail with IPCZ_RESULT_ALREADY_EXISTS.
  //
  // Unlike a plain Get() call, two-phase get operations allow the application
  // to read directly from portal memory, potentially reducing memory access
  // costs by eliminating redundant copying and caching.
  //
  // Like Get(), `num_ipcz_handles` and `num_os_handles` if non-null specify how
  // much capacity the caller has respectively in `ipcz_handles` and
  // `os_handles` to receive any handles from the retrieved parcel. If either
  // value is insufficient to hold the parcel's handles, BeginGet() returns
  // IPCZ_RESULT_RESOURCE_EXHAUSTED and updates the pointed-to values (if the
  // pointers are non-null) to reflect the required capacity for each.
  //
  // The input value of `num_data_bytes` is ignored. If `data` or
  // `num_data_bytes` is null and the available parcel has at least one byte of
  // data, this returns IPCZ_RESULT_RESOURCE_EXHAUSTED.
  //
  // Otherwise if `data` and `num_data_bytes` are non-null, a successful
  // BeginGet() updates them to expose portal memory for the application to
  // consume.
  //
  // `flags` is ignored and must be IPCZ_NO_FLAGS.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the two-phase get was successfully initiated. In this
  //        case both `*data` and `*num_data_bytes` are updated (if `data` and
  //        `num_data_bytes` were non-null) to describe the portal memory from
  //        which the application is free to read parcel data. Any handles in
  //        the parcel are populated in `ipcz_handles` and `os_handles` and
  //        the values of `*num_ipcz_handles` and `*num_os_handles` are updated
  //        to reflect how many of each were retrieved. This get operation
  //        remains in-progress until a corresponding EndGet() call is issued on
  //        `portal`.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `num_ipcz_handles`
  //        is non-null and `*num_ipcz_handles` is non-zero but `ipcz_handles`
  //        is null, or `num_os_handles` is non-null and `*num_os_handles` is
  //        non-zero but `os_handles` is null.
  //
  //    IPCZ_RESULT_RESOURCE_EXHAUSTED if the next available parcel has at least
  //        one data byte but `data` or `num_data_bytes` is null; if the next
  //        available parcel has at least one ipcz handle but `num_ipcz_handles`
  //        is null or `*num_ipcz_handles` is 0; or if the next available parcel
  //        has at least one OS handle but `num_os_handles` is null or
  //        `*num_os_handles` is 0. In any case, the value pointed to by any
  //        non-null pointer here (except `data`) is updated to reflect the
  //        required capacity for a BeginGet() retry to succeed.
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
                         const void** data,
                         uint32_t* num_data_bytes,
                         IpczHandle* ipcz_handles,
                         uint32_t* num_ipcz_handles,
                         IpczOSHandle* os_handles,
                         uint32_t* num_os_handles,
                         uint32_t flags,
                         const void* options);

  // Ends the two-phase get operation started by the most recent successful call
  // to BeginGet() on `portal`.
  //
  // `num_data_bytes_consumed` specifies the number of bytes actually read from
  // the buffer that was returned from the original BeginGet() call.
  //
  // If IPCZ_END_GET_ABORT is given in `flags` and there is a two-phase get
  // operation in progress on `portal`, all other arguments are ignored and the
  // pending operation is cancelled without consuming any data from the portal.
  // Note that any handles which were already consumed by the corresponding
  // BeginGet() remain property of the caller.
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
  //        `num_data_bytes_consumed` is larger than the capacity of the buffer
  //        originally returned by BeginGet().
  //
  //    IPCZ_RESULT_FAILED_PRECONDITION if there was no two-phase get operation
  //        in progress on `portal`.
  IpczResult (*EndGet)(IpczHandle portal,
                       uint32_t num_data_bytes_consumed,
                       IpczEndGetFlags flags,
                       const void* options);

  // Creates a new monitor to watch for specific conditions on the given portal.
  // The conditions to monitor are given by `descriptor->conditions`.
  //
  // Monitors are created in an inactive state, and an inactive monitor will
  // never invoke its handler. To activate a monitor, call ActivateMonitor.
  //
  // Whenever a monitor is active and a portal transitions to state which meets
  // the monitor's conditions, `descriptor->handler` is invoked with
  // `descriptor->context` from whichever thread changed the portal's state
  // accordingly. This invocation will also provide an IpczPortalStatus
  // structure to convey the last known status of the portal to the handler.
  // Only fields selected by `descriptor->status_fields` will be populated on
  // each invocation.
  //
  // A portal's state may be changed by any thread, including an internal ipcz
  // thread observing incoming parcels from out-of-process. Because of this,
  // applications should assume that `handler` may be invoked at any time on any
  // thread, and so it must always be thread-safe.
  //
  // If a relevant state change is elicited directly by the application calling
  // an ipcz function (for example, putting a parcel into the opposite portal
  // with Put()), any corresponding `handler` invocation will be deferred until
  // just before the stack unwinds from the outermost ipcz API invocation. This
  // avoids potential reentrancy issues in ipcz.
  //
  // Handlers do not need to support reentrancy either, as ipcz will never run
  // overlapping IpczMonitorEventHandlers. If an action taken by one handler
  // would elicit the invocation of another handler, the latter handler's
  // invocation is deferred until immediately after the first handler returns.
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the monitor was created successfully. The monitor must
  //        still be activated before it can invoke `handler` in response to any
  //        state changes on `portal`. Upon return, `*monitor` is populated with
  //        a handle to the new monitor object.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `portal` is invalid, `descriptor` is
  //        null or invalid, `conditions` is null or invalid, or `monitor` is
  //        null.
  IpczResult (*CreateMonitor)(IpczHandle portal,
                              const struct IpczMonitorDescriptor* descriptor,
                              uint32_t flags,
                              const void* options,
                              IpczHandle* monitor);

  // Activates `monitor` if possible.
  //
  // Note that a monitor can only be activated while its conditions are NOT met.
  // Otherwise this call will fail with an error indicating that some condition
  // is already met, and the value pointed to by `conditions` (if non-null) will
  // be updated with one or more flags indicating which condition(s) are already
  // met.
  //
  // If `status` is non-null, then it will also be populated with details of the
  // portal's current status, according to the monitor's interest in various
  // status fields given by its IpczMonitorDescriptor at creation time. Similar
  // to QueryPortalStatus(), status fields not explicitly requested by the
  // IpczMonitorDescriptor will not be populated here.
  //
  // Once a monitor is successfully activated, it can invoke its handler exactly
  // once, the next time the portal's state changes to satisfy one of the
  // monitor's conditions. This may even occur just before ActivateMonitor()
  // returns with IPCZ_RESULT_OK, so its important for the application's
  // activation logic to be synchronized against any side effects of the
  // handler's execution.
  //
  // Immediately before the handler is invoked in response to an event, the
  // monitor is automatically deactivated and it must be activated again by the
  // application before it will invoke `handler` again.
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the monitor was successfully activated. Its handler
  //        may be invoked any time after activation succeeds, even before this
  //        result is returned; although the invocation in that case would have
  //        to occur on some other thread.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `monitor` is invalid, or status is
  //        non-null and invalid.
  //
  //    IPCZ_RESULT_FAILED_PRECONDITION if at least one of the monitor's
  //        conditions is already met, such that activation would have caused
  //        an event to fire immediately. In this case `conditions` and
  //        `status`, if non-null, will be populated according to the
  //        description given above, allowing the application to deduce the
  //        precise reasons why activation could not succeed.
  IpczResult (*ActivateMonitor)(IpczHandle monitor,
                                uint32_t flags,
                                const void* options,
                                IpczMonitorConditionFlags* conditions,
                                struct IpczPortalStatus* status);

  // Destroys a portal monitor previously created by CreateMonitor().
  //
  // `flags` is ignored and must be 0.
  //
  // `options` is ignored and must be null.
  //
  // Returns:
  //
  //    IPCZ_RESULT_OK if the monitor was successfully destroyed. Once this
  //        result is returned, ipcz guarantees that the monitor's handler will
  //        no longer be invoked. If the handler is currently executing on
  //        another thread when this is called, this call will block until the
  //        handler terminates.
  //
  //    IPCZ_RESULT_INVALID_ARGUMENT if `monitor` is invalid`.
  //
  //    IPCZ_RESULT_FAILED_PRECONDITION if the calling thread is currently
  //        executing the monitor's handler. The monitor cannot be destroyed in
  //        this case because monitor destruction blocks on handler termination,
  //        and this would deadlock.
  IpczResult (*DestroyMonitor)(IpczHandle monitor,
                               uint32_t flags,
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