// Jackson Coxson

#include "idevice.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void print_usage(const char *program_name) {
  printf("Usage: %s <device_ip> <bundle_id> [pairing_file]\n", program_name);
  printf("Example: %s 10.0.0.1 com.example.app pairing.plist\n", program_name);
}

int main(int argc, char **argv) {
  // Initialize logger
  idevice_init_logger(Debug, Disabled, NULL);

  if (argc < 3) {
    print_usage(argv[0]);
    return 1;
  }

  const char *device_ip = argv[1];
  const char *bundle_id = argv[2];
  const char *pairing_file = argc > 3 ? argv[3] : "pairing_file.plist";

  /*****************************************************************
   * CoreDeviceProxy Setup
   *****************************************************************/
  printf("=== Setting up CoreDeviceProxy ===\n");

  // Create socket address
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(LOCKDOWN_PORT);
  if (inet_pton(AF_INET, device_ip, &addr.sin_addr) != 1) {
    fprintf(stderr, "Invalid IP address\n");
    return 1;
  }

  // Read pairing file
  IdevicePairingFile *pairing = NULL;
  IdeviceErrorCode err = idevice_pairing_file_read(pairing_file, &pairing);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to read pairing file: %d\n", err);
    return 1;
  }

  // Create TCP provider
  TcpProviderHandle *tcp_provider = NULL;
  err = idevice_tcp_provider_new((struct sockaddr *)&addr, pairing,
                                 "ProcessDebugTest", &tcp_provider);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to create TCP provider: %d\n", err);
    idevice_pairing_file_free(pairing);
    return 1;
  }

  // Connect to CoreDeviceProxy
  CoreDeviceProxyHandle *core_device = NULL;
  err = core_device_proxy_connect_tcp(tcp_provider, &core_device);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to connect to CoreDeviceProxy: %d\n", err);
    tcp_provider_free(tcp_provider);
    return 1;
  }
  tcp_provider_free(tcp_provider);

  // Get server RSD port
  uint16_t rsd_port;
  err = core_device_proxy_get_server_rsd_port(core_device, &rsd_port);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to get server RSD port: %d\n", err);
    core_device_proxy_free(core_device);
    return 1;
  }
  printf("Server RSD Port: %d\n", rsd_port);

  /*****************************************************************
   * Create TCP Tunnel Adapter
   *****************************************************************/
  printf("\n=== Creating TCP Tunnel Adapter ===\n");

  AdapterHandle *adapter = NULL;
  err = core_device_proxy_create_tcp_adapter(core_device, &adapter);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to create TCP adapter: %d\n", err);
    core_device_proxy_free(core_device);
    return 1;
  }

  // Connect to RSD port
  err = adapter_connect(adapter, rsd_port);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to connect to RSD port: %d\n", err);
    adapter_free(adapter);
    core_device_proxy_free(core_device);
    return 1;
  }
  printf("Successfully connected to RSD port\n");

  /*****************************************************************
   * XPC Device Setup
   *****************************************************************/
  printf("\n=== Setting up XPC Device ===\n");

  XPCDeviceAdapterHandle *xpc_device = NULL;
  err = xpc_device_new(adapter, &xpc_device);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to create XPC device: %d\n", err);
    adapter_free(adapter);
    core_device_proxy_free(core_device);
    return 1;
  }

  // Get DebugProxy service
  XPCServiceHandle *debug_service = NULL;
  err = xpc_device_get_service(
      xpc_device, "com.apple.internal.dt.remote.debugproxy", &debug_service);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to get debug proxy service: %d\n", err);
    return 1;
  }

  // Get ProcessControl service
  XPCServiceHandle *pc_service = NULL;
  err = xpc_device_get_service(xpc_device, "com.apple.instruments.dtservicehub",
                               &pc_service);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to get process control service: %d\n", err);
    xpc_device_free(xpc_device);
    core_device_proxy_free(core_device);
    return 1;
  }

  /*****************************************************************
   * Process Control - Launch App
   *****************************************************************/
  printf("\n=== Launching App ===\n");

  // Get the adapter back from XPC device
  AdapterHandle *pc_adapter = NULL;
  err = xpc_device_adapter_into_inner(xpc_device, &pc_adapter);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to extract adapter: %d\n", err);
    xpc_device_free(xpc_device);
    core_device_proxy_free(core_device);
    return 1;
  }

  // Connect to process control port
  err = adapter_connect(pc_adapter, pc_service->port);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to connect to process control port: %d\n", err);
    adapter_free(pc_adapter);
    xpc_service_free(pc_service);
    core_device_proxy_free(core_device);
    return 1;
  }
  printf("Successfully connected to process control port\n");

  // Create RemoteServerClient
  RemoteServerAdapterHandle *remote_server = NULL;
  err = remote_server_adapter_new(pc_adapter, &remote_server);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to create remote server: %d\n", err);
    adapter_free(pc_adapter);
    xpc_service_free(pc_service);
    core_device_proxy_free(core_device);
    return 1;
  }

  // Create ProcessControlClient
  ProcessControlAdapterHandle *process_control = NULL;
  err = process_control_new(remote_server, &process_control);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to create process control client: %d\n", err);
    remote_server_free(remote_server);
    xpc_service_free(pc_service);
    core_device_proxy_free(core_device);
    return 1;
  }

  // Launch application
  uint64_t pid;
  err = process_control_launch_app(process_control, bundle_id, NULL, 0, NULL, 0,
                                   true, false, &pid);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to launch app: %d\n", err);
    process_control_free(process_control);
    remote_server_free(remote_server);
    xpc_service_free(pc_service);
    core_device_proxy_free(core_device);
    return 1;
  }
  printf("Successfully launched app with PID: %" PRIu64 "\n", pid);

  /*****************************************************************
   * Debug Proxy - Attach to Process
   *****************************************************************/
  printf("\n=== Attaching Debugger ===\n");

  // Get the adapter back from the remote server
  AdapterHandle *debug_adapter = NULL;
  err = remote_server_adapter_into_inner(remote_server, &debug_adapter);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to extract adapter: %d\n", err);
    xpc_service_free(debug_service);
    process_control_free(process_control);
    remote_server_free(remote_server);
    xpc_service_free(pc_service);
    core_device_proxy_free(core_device);
    return 1;
  }

  // Connect to debug proxy port
  err = adapter_connect(debug_adapter, debug_service->port);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to connect to debug proxy port: %d\n", err);
    adapter_free(debug_adapter);
    xpc_service_free(debug_service);
    process_control_free(process_control);
    remote_server_free(remote_server);
    xpc_service_free(pc_service);
    core_device_proxy_free(core_device);
    return 1;
  }
  printf("Successfully connected to debug proxy port\n");

  // Create DebugProxyClient
  DebugProxyAdapterHandle *debug_proxy = NULL;
  err = debug_proxy_adapter_new(debug_adapter, &debug_proxy);
  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to create debug proxy client: %d\n", err);
    adapter_free(debug_adapter);
    xpc_service_free(debug_service);
    process_control_free(process_control);
    remote_server_free(remote_server);
    xpc_service_free(pc_service);
    core_device_proxy_free(core_device);
    return 1;
  }

  // Send vAttach command with PID in hex
  char attach_command[64];
  snprintf(attach_command, sizeof(attach_command), "vAttach;%" PRIx64, pid);

  DebugserverCommandHandle *attach_cmd =
      debugserver_command_new(attach_command, NULL, 0);
  if (attach_cmd == NULL) {
    fprintf(stderr, "Failed to create attach command\n");
    debug_proxy_free(debug_proxy);
    adapter_free(debug_adapter);
    xpc_service_free(debug_service);
    process_control_free(process_control);
    remote_server_free(remote_server);
    xpc_service_free(pc_service);
    core_device_proxy_free(core_device);
    return 1;
  }

  char *attach_response = NULL;
  err = debug_proxy_send_command(debug_proxy, attach_cmd, &attach_response);
  debugserver_command_free(attach_cmd);

  if (err != IdeviceSuccess) {
    fprintf(stderr, "Failed to attach to process: %d\n", err);
  } else if (attach_response != NULL) {
    printf("Attach response: %s\n", attach_response);
    idevice_string_free(attach_response);
  }

  // Send detach command
  DebugserverCommandHandle *detach_cmd = debugserver_command_new("D", NULL, 0);
  if (detach_cmd == NULL) {
    fprintf(stderr, "Failed to create detach command\n");
  } else {
    char *detach_response = NULL;
    err = debug_proxy_send_command(debug_proxy, detach_cmd, &detach_response);
    err = debug_proxy_send_command(debug_proxy, detach_cmd, &detach_response);
    err = debug_proxy_send_command(debug_proxy, detach_cmd, &detach_response);
    debugserver_command_free(detach_cmd);

    if (err != IdeviceSuccess) {
      fprintf(stderr, "Failed to detach from process: %d\n", err);
    } else if (detach_response != NULL) {
      printf("Detach response: %s\n", detach_response);
      idevice_string_free(detach_response);
    }
  }

  /*****************************************************************
   * Cleanup
   *****************************************************************/
  debug_proxy_free(debug_proxy);
  xpc_service_free(debug_service);

  printf("\nDebug session completed\n");
  return 0;
}
