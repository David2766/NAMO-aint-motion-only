#include "dashboard_handler.h"

#include "dashboard_assets.h"
#include "http_response.h"

namespace esphome {
namespace radar_api_server {

bool DashboardHandler::can_handle(AsyncWebServerRequest *request) const {
  if (request->method() != HTTP_GET)
    return false;

  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  auto url = request->url_to(url_buf);
  return url == "/dashboard" || url == "/dashboard/" || url == "/dashboard/assets/dashboard.js" ||
         url == "/dashboard/assets/dashboard.css";
}

bool DashboardHandler::handle(AsyncWebServerRequest *request) const {
  if (!this->can_handle(request))
    return false;

  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  auto url = request->url_to(url_buf);

  if (url == "/dashboard/assets/dashboard.js") {
    http_response::send_gzip_asset(request, dashboard_assets::DASHBOARD_JS_CONTENT_TYPE,
                                   dashboard_assets::DASHBOARD_JS_GZ, dashboard_assets::DASHBOARD_JS_GZ_SIZE);
    return true;
  }
  if (url == "/dashboard/assets/dashboard.css") {
    http_response::send_gzip_asset(request, dashboard_assets::DASHBOARD_CSS_CONTENT_TYPE,
                                   dashboard_assets::DASHBOARD_CSS_GZ, dashboard_assets::DASHBOARD_CSS_GZ_SIZE);
    return true;
  }

  http_response::send_gzip_asset(request, dashboard_assets::INDEX_HTML_CONTENT_TYPE, dashboard_assets::INDEX_HTML_GZ,
                                 dashboard_assets::INDEX_HTML_GZ_SIZE);
  return true;
}

}  // namespace radar_api_server
}  // namespace esphome
