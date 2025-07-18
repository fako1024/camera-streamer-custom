extern "C" {

#include "util/http/http.h"
#include "util/opts/fourcc.h"
#include "util/opts/control.h"
#include "device/buffer_list.h"
#include "device/buffer_lock.h"
#include "device/camera/camera.h"
#include "output/rtsp/rtsp.h"
#include "output/webrtc/webrtc.h"
#include "output/output.h"
#include "version.h"

extern camera_t *camera;
extern http_server_options_t http_options;
extern rtsp_options_t rtsp_options;
extern webrtc_options_t webrtc_options;

};

#include <nlohmann/json.hpp>
#include "third_party/magic_enum/include/magic_enum.hpp"

static nlohmann::json serialize_buf_list(buffer_list_t *buf_list)
{
  if (!buf_list)
    return false;

  nlohmann::json output;
  output["name"] = buf_list->name;
  output["width"] = buf_list->fmt.width;
  output["height"] = buf_list->fmt.height;
  output["format"] = fourcc_to_string(buf_list->fmt.format).buf;
  output["nbufs"] = buf_list->nbufs;

  return output;
}

static nlohmann::json serialize_buf_lock(buffer_lock_t *buf_lock)
{
  if (!buf_lock)
    return false;

  nlohmann::json output;
  output["name"] = buf_lock->name;
  output["enabled"] = (buf_lock->buf_list != NULL);

  if (buf_lock->buf_list) {
    output["width"] = buf_lock->buf_list->fmt.width;
    output["height"] = buf_lock->buf_list->fmt.height;
    output["source"] = buf_lock->buf_list->name;
    output["frames"] = buf_lock->counter;
    output["refs"] = buf_lock->refs;
    output["dropped"] = buf_lock->dropped;
  }
  return output;
}

static const char *strip_prefix(const char *str, const char *prefix)
{
  if (strstr(str, prefix) == str) {
    return str + strlen(prefix);
  }
  return str;
}

static std::string std_device_option_normalize(std::string key)
{
  key.resize(device_option_normalize_name(key.data(), key.data()));
  return key;
}

static int device_options_callback(device_option_t *option, void *opaque)
{
  auto key = std_device_option_normalize(option->name);
  nlohmann::json &device = *(nlohmann::json*)opaque;
  nlohmann::json &node = option->flags.read_only ?
    device["properties"][key] :
    device["options"][key];

  node["name"] = option->name;
  node["type"] = strip_prefix(
    std::string(magic_enum::enum_name(option->type)).c_str(),
    "device_option_type_");

  if (option->elems > 0)
    node["elems"] = option->elems;
  if (option->description[0])
    node["description"] = option->description;
  if (option->value[0])
    node["value"] = option->value;

  for (int i = 0; i < option->menu_items; i++) {
    char buf[64];
    sprintf(buf, "%d", option->menu[i].id);
    node["menu"][buf] = option->menu[i].name;
  }
  return 0;
}

static nlohmann::json devices_status_json()
{
  nlohmann::json devices;

  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!camera->devices[i])
      continue;

    device_t *device = camera->devices[i];
    nlohmann::json device_json;
    device_json["name"] = device->name;
    device_json["path"] = device->path;
    device_json["allow_dma"] = device->opts.allow_dma;
    device_json["output"] = serialize_buf_list(device->output_list);
    for (int j = 0; j < device->n_capture_list; j++) {
      device_json["captures"][j] = serialize_buf_list(device->capture_lists[j]);
    }

    device_dump_options2(device, device_options_callback, &device_json);
    devices += device_json;
  }

  return devices;
}

static nlohmann::json links_status_json()
{
  nlohmann::json links;

  for (int i = 0; i < camera->nlinks; i++) {
    link_t *link = &camera->links[i];

    nlohmann::json link_json;
    link_json["source"] = link->capture_list->name;
    for (int j = 0; j < link->n_output_lists; j++) {
      link_json["sinks"][j] = link->output_lists[j]->name;
    }
    for (int j = 0; j < link->n_callbacks; j++) {
      link_json["callbacks"][j] = link->callbacks[j].name;
    }
    links += link_json;
  }

  return links;
}

static std::string strip_host_port(const char *host)
{
  const char *sep = strchr(host, ':');

  return sep ? std::string(host, sep) : host;
}

static nlohmann::json get_url(bool running, const char *output, const char *protocol, const char *host, int port, const char *path)
{
  nlohmann::json endpoint;

  endpoint["enabled"] = running;

  if (running) {
    endpoint["output"] = output;
    endpoint["uri"] = std::string(protocol) + "://" + strip_host_port(host) + ":" + std::to_string(port) + path;
  }

  return endpoint;
}

extern "C" void camera_status_json(http_worker_t *worker, FILE *stream)
{
  nlohmann::json message;

  message["version"] = GIT_VERSION;
  message["revision"] = GIT_REVISION;

  message["outputs"]["preview"] = serialize_buf_lock(&preview_lock);
  message["outputs"]["snapshot"] = serialize_buf_lock(&snapshot_lock);
  message["outputs"]["stream"] = serialize_buf_lock(&stream_lock);
  message["outputs"]["video"] = serialize_buf_lock(&video_lock);

  message["devices"] = devices_status_json();
  message["links"] = links_status_json();

  message["endpoints"]["rtsp"] = get_url(video_lock.buf_list != NULL && rtsp_options.running, "video", "rtsp", worker->host, rtsp_options.port, "/stream.h264");
  message["endpoints"]["webrtc"] = get_url(video_lock.buf_list != NULL && webrtc_options.running, "video", "http", worker->host, http_options.port, "/webrtc");
  message["endpoints"]["video"] = get_url(video_lock.buf_list != NULL, "video", "http", worker->host, http_options.port, "/video");
  message["endpoints"]["stream"] = get_url(stream_lock.buf_list != NULL, "stream", "http", worker->host, http_options.port, "/stream");
  message["endpoints"]["preview"] = get_url(preview_lock.buf_list != NULL, "preview", "http", worker->host, http_options.port, "/preview");
  message["endpoints"]["snapshot"] = get_url(snapshot_lock.buf_list != NULL, "snapshot", "http", worker->host, http_options.port, "/snapshot");

  if (rtsp_options.running) {
    message["endpoints"]["rtsp"]["clients"] = rtsp_options.clients;
    message["endpoints"]["rtsp"]["truncated"] = rtsp_options.truncated;
    message["endpoints"]["rtsp"]["frames"] = rtsp_options.frames;
    message["endpoints"]["rtsp"]["dropped"] = rtsp_options.dropped;
  }

  http_write_response(stream, "200 OK", "application/json", message.dump().c_str(), 0);
}
