/*
Copyright (c) 2013, Silas Parker
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.
    Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
    The name of Silas Parker may not be used to endorse or promote products
    derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <Winsock2.h>
#endif /* WIN32 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <list>

#ifdef WIN32
#define snprintf _snprintf
#endif

#include "cJSON.h"

#include <scssdk_telemetry.h>
#include <eurotrucks2/scssdk_telemetry_eut2.h>



#define LOG_PREFIX "[DASHv2] "
#define BUFFER_SIZE (256)
#define BIND_PORT (21212)

// Game log file
static scs_log_t game_log = 0;


#ifdef WIN32
static bool wsa_initialised = false;

static bool wsa_init()
{
  if (wsa_initialised)
    return true;
  
  WORD wVersionRequested = MAKEWORD(2, 0);
  WSADATA wsaData;
  if (WSAStartup(wVersionRequested, &wsaData) != 0)
  {
    return false;
  }
  wsa_initialised = true;
  return true;
}

static void wsa_close()
{
  if (wsa_initialised)
    WSACleanup();
  wsa_initialised = false;
}
#else /* WIN32 */
static bool wsa_initialised = true;
static bool wsa_init() { return true; }
static void wsa_close() {}
#define closesocket(X) close(X)
#endif /* WIN32 */




int listen_socket = -1;
std::list<int> client_sockets;


static unsigned long last_update = 0;
bool send_update = false;
static cJSON *root, *telem, *config;

SCSAPI_VOID telemetry_frame_start(const scs_event_t /*event*/, const void *const /*event_info*/, const scs_context_t /*context*/)
{
  if (listen_socket == -1)
    return;
  
  const unsigned long now = GetTickCount();
  const unsigned long diff = now - last_update;
  if (diff < 500)
  {
    send_update = false;
    return;
  }
  send_update = true;
  
  // Check for new connections
  int new_socket;
  sockaddr_in from_addr;
  int from_addr_size = sizeof(from_addr);
  while ((new_socket = accept(listen_socket, (sockaddr*)&from_addr, &from_addr_size)) >= 0)
  {
    client_sockets.push_back(new_socket);
    unsigned long ip = ntohl(from_addr.sin_addr.s_addr);
    char logbuffer[64];
    snprintf(logbuffer, sizeof(logbuffer), LOG_PREFIX "Connection from %d.%d.%d.%d:%d",
        (ip>>24)&0xFF, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF, ntohs(from_addr.sin_port));
    game_log(SCS_LOG_TYPE_message, logbuffer);
  }
  if (WSAGetLastError() != WSAEWOULDBLOCK)
  {
    char logbuffer[64];
    snprintf(logbuffer, sizeof(logbuffer), LOG_PREFIX "ERROR: %d", WSAGetLastError());
    game_log(SCS_LOG_TYPE_message, logbuffer);
  }
}


SCSAPI_VOID telemetry_frame_end(const scs_event_t /*event*/, const void *const /*event_info*/, const scs_context_t /*context*/)
{
  if ((listen_socket == -1) || (!send_update))
    return;
  
  // Serialise to JSON
  
  
  //char* data = cJSON_Print(root);
  char* data = cJSON_PrintUnformatted(root);
  size_t data_len = strlen(data);
  
  assert(data_len < 65535);
  
  unsigned char header[4] = {
    1,  // Message Version
    0,  // Reserved
    (data_len >> 8) & 0xFF, // Length High
    data_len & 0xFF  // Length Low
  };
  
  for (std::list<int>::iterator it = client_sockets.begin(); it != client_sockets.end(); ++it)
  {
    if (
      (send(*it, (char*)header, sizeof(header), 0) < 0) ||
      (send(*it, data, data_len, 0) < 0))
    {
      closesocket(*it);
      it = client_sockets.erase(it); // Moves to the socket after
      --it; // Don't skip the socket with the for loop ++
    }
  }
  free(data);  
}

void add_item_by_path(cJSON* parent, const char* name, cJSON* item)
{
  assert(parent);
  assert(name);
  assert(item);
  const char* end = strchr(name, '.');
  if (end == NULL)
  {
    if (cJSON_GetObjectItem(parent, name) == NULL)
    {
      cJSON_AddItemToObject(parent, name, item);
    }
    else
    {
      cJSON_ReplaceItemInObject(parent, name, item);
    }
    return;
  }
  
  std::string part_name(name, end);
  cJSON* part_obj = cJSON_GetObjectItem(parent, part_name.c_str());
  if (part_obj == NULL)
  {
    cJSON_AddItemToObject(parent, part_name.c_str(), part_obj = cJSON_CreateObject());
  }
  add_item_by_path(part_obj, end + 1, item);
}

void add_value_by_path(cJSON* parent, const char* name, const scs_value_t* value)
{
  switch (value->type)
  {
    case SCS_VALUE_TYPE_bool:
      add_item_by_path(parent, name, cJSON_CreateBool(value->value_bool.value));
      break;
    case SCS_VALUE_TYPE_s32:
      add_item_by_path(parent, name, cJSON_CreateNumber(value->value_s32.value));
      break;
    case SCS_VALUE_TYPE_u32:
      add_item_by_path(parent, name, cJSON_CreateNumber(value->value_u32.value));
      break;
    case SCS_VALUE_TYPE_u64:
      add_item_by_path(parent, name, cJSON_CreateNumber(value->value_u64.value));
      break;
    case SCS_VALUE_TYPE_float:
      add_item_by_path(parent, name, cJSON_CreateNumber(value->value_float.value));
      break;
    case SCS_VALUE_TYPE_double:
      add_item_by_path(parent, name, cJSON_CreateNumber(value->value_double.value));
      break;
    case SCS_VALUE_TYPE_fvector:
      add_item_by_path(parent, name, cJSON_CreateFloatArray(&(value->value_fvector.x), 3));
      break;
    case SCS_VALUE_TYPE_dvector:
      add_item_by_path(parent, name, cJSON_CreateDoubleArray(&(value->value_dvector.x), 3));
      break;
    case SCS_VALUE_TYPE_euler:
      add_item_by_path(parent, name, cJSON_CreateFloatArray(&(value->value_euler.heading), 3));
      break;
    case SCS_VALUE_TYPE_fplacement:
      add_item_by_path(parent, name, cJSON_CreateFloatArray(&(value->value_fplacement.position.x), 6));
      break;
    case SCS_VALUE_TYPE_dplacement:
      double values[6];
      memcpy(values, &(value->value_dplacement.position.x), 3 * sizeof(double));
      values[3] = value->value_dplacement.orientation.heading;
      values[4] = value->value_dplacement.orientation.pitch;
      values[5] = value->value_dplacement.orientation.roll;
      add_item_by_path(parent, name, cJSON_CreateDoubleArray(values, 6));
      break;
    case SCS_VALUE_TYPE_string:
      add_item_by_path(parent, name, cJSON_CreateString(value->value_string.value));
      break;
    default:
      break;
      // TODO: ADD ALL TYPES
  }
}

SCSAPI_VOID telemetry_store_value(const scs_string_t name, const scs_u32_t index, const scs_value_t *const value, const scs_context_t context)
{
	assert(value);
  if (telem == NULL)
    return;
  add_value_by_path(telem, name, value);
}

SCSAPI_VOID telemetry_configuration(const scs_event_t /*event*/, const void *const event_info, const scs_context_t /*context*/)
{
  const struct scs_telemetry_configuration_t *const info = static_cast<const scs_telemetry_configuration_t *>(event_info);
  
  char fullname[256];
  for (const scs_named_value_t *current = info->attributes; current->name; ++current)
  {
    fullname[0] = '\0';
    strcat(fullname, info->id);
    strcat(fullname, ".");
    strcat(fullname, current->name);
    add_value_by_path(config, fullname, &(current->value));
  }
}

SCSAPI_VOID game_start(const scs_event_t /*event*/, const void *const /*event_info*/, const scs_context_t /*context*/)
{
  strcpy(cJSON_GetObjectItem(root, "state")->valuestring, "drive");
}

SCSAPI_VOID game_pause(const scs_event_t /*event*/, const void *const /*event_info*/, const scs_context_t /*context*/)
{
  strcpy(cJSON_GetObjectItem(root, "state")->valuestring, "pause");
}


SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params)
{
	if (version != SCS_TELEMETRY_VERSION_1_00) {
		return SCS_RESULT_unsupported;
	}

	const scs_telemetry_init_params_v100_t *const version_params = static_cast<const scs_telemetry_init_params_v100_t *>(params);
  game_log = version_params->common.log;
  
  game_log(SCS_LOG_TYPE_message, LOG_PREFIX "Plugin initialising");
    
  // Register for in game events
  bool registered =
    (version_params->register_for_event(
      SCS_TELEMETRY_EVENT_frame_start, telemetry_frame_start, NULL) == SCS_RESULT_ok) &&
    (version_params->register_for_event(
      SCS_TELEMETRY_EVENT_frame_end, telemetry_frame_end, NULL) == SCS_RESULT_ok) &&
    (version_params->register_for_event(
      SCS_TELEMETRY_EVENT_paused, game_pause, NULL) == SCS_RESULT_ok) &&
    (version_params->register_for_event(
      SCS_TELEMETRY_EVENT_started, game_start, NULL) == SCS_RESULT_ok) &&
    (version_params->register_for_event(
      SCS_TELEMETRY_EVENT_configuration, telemetry_configuration, NULL) == SCS_RESULT_ok);
  
  // Register for truck channels
  
#define REG_CHAN(NS,CHANNEL,TYPE) \
  registered &= (version_params->register_for_channel( \
  SCS_TELEMETRY_ ## NS ## CHANNEL_ ## CHANNEL, SCS_U32_NIL, SCS_VALUE_TYPE_ ## TYPE, \
  SCS_TELEMETRY_CHANNEL_FLAG_none, telemetry_store_value, NULL) == SCS_RESULT_ok)

#define REG_COMMON_CHAN(CHANNEL,TYPE) REG_CHAN(,CHANNEL,TYPE)

#define REG_TRUCK_CHAN(CHANNEL,TYPE) REG_CHAN(TRUCK_,CHANNEL,TYPE)

#define REG_TRAILER_CHAN(CHANNEL,TYPE) REG_CHAN(TRAILER_,CHANNEL,TYPE)

  REG_COMMON_CHAN(local_scale,    float);
  REG_COMMON_CHAN(game_time,      u32);
  REG_COMMON_CHAN(next_rest_stop, s32);
  
  
  REG_TRUCK_CHAN(world_placement,               dplacement);
  REG_TRUCK_CHAN(local_linear_velocity,         fvector);
  REG_TRUCK_CHAN(local_angular_velocity,        fvector);
  REG_TRUCK_CHAN(local_linear_acceleration,     fvector);
  REG_TRUCK_CHAN(local_angular_acceleration,    fvector);
  REG_TRUCK_CHAN(cabin_offset,                  fplacement);
  REG_TRUCK_CHAN(cabin_angular_velocity,        fvector);
  REG_TRUCK_CHAN(cabin_angular_acceleration,    fvector);
  REG_TRUCK_CHAN(head_offset,                   fplacement);
  REG_TRUCK_CHAN(speed,                         float);
  REG_TRUCK_CHAN(engine_rpm,                    float);
  REG_TRUCK_CHAN(engine_gear,                   s32);
  REG_TRUCK_CHAN(displayed_gear,                s32);
  REG_TRUCK_CHAN(input_steering,                float);
  REG_TRUCK_CHAN(input_throttle,                float);
  REG_TRUCK_CHAN(input_brake,                   float);
  REG_TRUCK_CHAN(input_clutch,                  float);
  REG_TRUCK_CHAN(effective_steering,            float);
  REG_TRUCK_CHAN(effective_throttle,            float);
  REG_TRUCK_CHAN(effective_brake,               float);
  REG_TRUCK_CHAN(effective_clutch,              float);
  REG_TRUCK_CHAN(cruise_control,                float);
  REG_TRUCK_CHAN(hshifter_slot,                 u32);
  // hshifter_selector
  REG_TRUCK_CHAN(parking_brake,                 bool);
  REG_TRUCK_CHAN(motor_brake,                   bool);
  REG_TRUCK_CHAN(retarder_level,                u32);
  REG_TRUCK_CHAN(brake_air_pressure,            float);
  REG_TRUCK_CHAN(brake_air_pressure_warning,    bool);
  REG_TRUCK_CHAN(brake_air_pressure_emergency,  bool);
  REG_TRUCK_CHAN(brake_temperature,             float);
  REG_TRUCK_CHAN(fuel,                          float);
  REG_TRUCK_CHAN(fuel_warning,                  bool);
  REG_TRUCK_CHAN(fuel_average_consumption,      float);
  REG_TRUCK_CHAN(fuel_range,                    float);
  REG_TRUCK_CHAN(adblue,                        float);
  REG_TRUCK_CHAN(adblue_warning,                bool);
  //REG_TRUCK_CHAN(adblue_average_consumption,    float);
  REG_TRUCK_CHAN(oil_pressure,                  float);
  REG_TRUCK_CHAN(oil_pressure_warning,          bool);
  REG_TRUCK_CHAN(oil_temperature,               float);
  REG_TRUCK_CHAN(water_temperature,             float);
  REG_TRUCK_CHAN(water_temperature_warning,     bool);
  REG_TRUCK_CHAN(battery_voltage,               float);
  REG_TRUCK_CHAN(battery_voltage_warning,       bool);
  REG_TRUCK_CHAN(electric_enabled,              bool);
  REG_TRUCK_CHAN(engine_enabled,                bool);
  REG_TRUCK_CHAN(lblinker,                      bool);
  REG_TRUCK_CHAN(rblinker,                      bool);
  REG_TRUCK_CHAN(light_lblinker,                bool);
  REG_TRUCK_CHAN(light_rblinker,                bool);
  REG_TRUCK_CHAN(light_parking,                 bool);
  REG_TRUCK_CHAN(light_low_beam,                bool);
  REG_TRUCK_CHAN(light_high_beam,               bool);
  REG_TRUCK_CHAN(light_aux_front,               u32);
  REG_TRUCK_CHAN(light_aux_roof,                u32);
  REG_TRUCK_CHAN(light_beacon,                  bool);
  REG_TRUCK_CHAN(light_brake,                   bool);
  REG_TRUCK_CHAN(light_reverse,                 bool);
  REG_TRUCK_CHAN(wipers,                        bool);
  REG_TRUCK_CHAN(dashboard_backlight,           float);
  REG_TRUCK_CHAN(wear_engine,                   float);
  REG_TRUCK_CHAN(wear_transmission,             float);
  REG_TRUCK_CHAN(wear_cabin,                    float);
  REG_TRUCK_CHAN(wear_chassis,                  float);
  REG_TRUCK_CHAN(wear_wheels,                   float);
  REG_TRUCK_CHAN(odometer,                      float);
  REG_TRUCK_CHAN(navigation_distance,           float);
  REG_TRUCK_CHAN(navigation_time,               float);
  REG_TRUCK_CHAN(navigation_speed_limit,        float);
  
  REG_TRAILER_CHAN(connected,                     bool);
  REG_TRAILER_CHAN(world_placement,               dplacement);
  REG_TRAILER_CHAN(local_linear_velocity,         fvector);
  REG_TRAILER_CHAN(local_angular_velocity,        fvector);
  REG_TRAILER_CHAN(local_linear_acceleration,     fvector);
  REG_TRAILER_CHAN(local_angular_acceleration,    fvector);
  REG_TRAILER_CHAN(wear_chassis,                  float);
  
  if (!registered)
  {
    game_log(SCS_LOG_TYPE_error, LOG_PREFIX "Unable to register callbacks");
		return SCS_RESULT_generic_error;
  }
  
  root = cJSON_CreateObject();
  if (root == NULL)
  {
    game_log(SCS_LOG_TYPE_error, LOG_PREFIX "Failed to create root JSON object");
		return SCS_RESULT_generic_error;
  }
  
  cJSON_AddItemToObject(root, "telemetry", telem = cJSON_CreateObject());
  if (telem == NULL)
  {
    game_log(SCS_LOG_TYPE_error, LOG_PREFIX "Failed to create telemetry JSON object");
		return SCS_RESULT_generic_error;
  }
  
  cJSON_AddItemToObject(root, "config", config = cJSON_CreateObject());
  if (config == NULL)
  {
    game_log(SCS_LOG_TYPE_error, LOG_PREFIX "Failed to create config JSON object");
		return SCS_RESULT_generic_error;
  }
  
  cJSON_AddStringToObject(root, "state", "startup");

  
  if (!wsa_init())
  {
    game_log(SCS_LOG_TYPE_error, LOG_PREFIX "Failed to load WSA");
		return SCS_RESULT_generic_error;
  }
  
  if (listen_socket == -1)
  {
    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket == -1)
    {
      game_log(SCS_LOG_TYPE_error, LOG_PREFIX "Failed to create listening socket");
      wsa_close();
      return SCS_RESULT_generic_error;
    }
    
#ifdef WIN32
    unsigned long non_blocking = 1;
    if (ioctlsocket(listen_socket, FIONBIO, &non_blocking) != 0)
#else
    if (fcntl(listen_socket, F_SETFL, O_NONBLOCK) != 0)
#endif
    {
      game_log(SCS_LOG_TYPE_error, LOG_PREFIX "Failed to set socket non blocking");
      closesocket(listen_socket);
      wsa_close();
      return SCS_RESULT_generic_error;
    }
    
    static const int yes = 1;
    if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes)) != 0)
    {
      game_log(SCS_LOG_TYPE_error, LOG_PREFIX "Failed to set socket option REUSEADDR");
      closesocket(listen_socket);
      wsa_close();
      return SCS_RESULT_generic_error;
    }
    
    struct sockaddr_in bind_addr;
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(BIND_PORT);
    bind_addr.sin_addr.s_addr = 0;
    
    if (bind(listen_socket, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0)
    {
      game_log(SCS_LOG_TYPE_error, LOG_PREFIX "Failed to bind listening port");
      closesocket(listen_socket);
      wsa_close();
      return SCS_RESULT_generic_error;
    }
    
    if (listen(listen_socket, 10) != 0)
    {
      game_log(SCS_LOG_TYPE_error, LOG_PREFIX "Failed to start listening");
      closesocket(listen_socket);
      wsa_close();
      return SCS_RESULT_generic_error;
    }
    
    char logbuffer[64];
    snprintf(logbuffer, sizeof(logbuffer), LOG_PREFIX "Listening on port %d", BIND_PORT);
    game_log(SCS_LOG_TYPE_message, logbuffer);
  } 
  
  
  
  game_log(SCS_LOG_TYPE_message, LOG_PREFIX "Plugin initialised");
  
  return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
  if (listen_socket > 0)
  {
    closesocket(listen_socket);
    listen_socket = -1;
  }
  wsa_close();
}

#ifdef WIN32
BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason_for_call, LPVOID /*reseved*/)
{
  if (reason_for_call == DLL_PROCESS_DETACH)
  {
    if (listen_socket > 0)
    {
      closesocket(listen_socket);
      listen_socket = -1;
    }
    wsa_close();
	}
	return TRUE;
}
#endif
