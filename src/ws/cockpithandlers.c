/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpithandlers.h"

#include "cockpitbranding.h"
#include "cockpitchannelresponse.h"
#include "cockpitchannelsocket.h"
#include "cockpitwebservice.h"
#include "cockpitws.h"

#include "common/cockpitconf.h"
#include "common/cockpitjson.h"
#include "common/cockpitwebcertificate.h"
#include "common/cockpitwebinject.h"

#include "websocket/websocket.h"

#include <json-glib/json-glib.h>

#include <gio/gio.h>
#include <glib/gi18n.h>

#include <string.h>

/* For overriding during tests */
const gchar *cockpit_ws_shell_component = "/shell/index.html";

static gchar *
locate_selfsign_ca (void)
{
  g_autofree gchar *cert_path = NULL;
  gchar *ca_path = NULL;
  gchar *error = NULL;

  cert_path = cockpit_certificate_locate (true, &error);
  if (cert_path && g_str_has_suffix (cert_path, "/0-self-signed.cert"))
    {
      g_autofree gchar *dir = g_path_get_dirname (cert_path);
      ca_path = g_build_filename (dir, "0-self-signed-ca.pem", NULL);
      if (!g_file_test (ca_path, G_FILE_TEST_EXISTS))
        {
          g_free (ca_path);
          ca_path = NULL;
        }
    }

  return ca_path;
}

static void
on_web_socket_noauth (WebSocketConnection *connection,
                      gpointer data)
{
  GBytes *payload;
  GBytes *prefix;

  g_debug ("closing unauthenticated web socket");

  payload = cockpit_transport_build_control ("command", "init", "problem", "no-session", NULL);
  prefix = g_bytes_new_static ("\n", 1);

  web_socket_connection_send (connection, WEB_SOCKET_DATA_TEXT, prefix, payload);
  web_socket_connection_close (connection, WEB_SOCKET_CLOSE_GOING_AWAY, "no-session");

  g_bytes_unref (prefix);
  g_bytes_unref (payload);
}

static void
handle_noauth_socket (GIOStream *io_stream,
                      const gchar *path,
                      GHashTable *headers,
                      GByteArray *input_buffer,
                      gboolean for_tls_proxy)
{
  WebSocketConnection *connection;

  connection = cockpit_web_service_create_socket (NULL, path, io_stream, headers, input_buffer, for_tls_proxy);

  g_signal_connect (connection, "open", G_CALLBACK (on_web_socket_noauth), NULL);

  /* Unreferences connection when it closes */
  g_signal_connect (connection, "close", G_CALLBACK (g_object_unref), NULL);
}

/* Called by @server when handling HTTP requests to /cockpit/socket */
gboolean
cockpit_handler_socket (CockpitWebServer *server,
                        const gchar *original_path,
                        const gchar *path,
                        const gchar *method,
                        GIOStream *io_stream,
                        GHashTable *headers,
                        GByteArray *input,
                        CockpitHandlerData *ws)
{
  CockpitWebService *service = NULL;
  const gchar *segment = NULL;

  /*
   * Socket requests should come in on /cockpit/socket or /cockpit+app/socket.
   * However older javascript may connect on /socket, so we continue to support that.
   */

  if (path && path[0])
    segment = strchr (path + 1, '/');
  if (!segment)
    segment = path;

  if (!segment || !g_str_equal (segment, "/socket"))
    return FALSE;

  /* don't support HEAD on a socket, it makes little sense */
  if (g_strcmp0 (method, "GET") != 0)
      return FALSE;

  if (headers)
    service = cockpit_auth_check_cookie (ws->auth, path, headers);
  if (service)
    {
      cockpit_web_service_socket (service, path, io_stream, headers, input,
                                  cockpit_web_server_get_flags (server) & COCKPIT_WEB_SERVER_FOR_TLS_PROXY);
      g_object_unref (service);
    }
  else
    {
      handle_noauth_socket (io_stream, path, headers, input,
                            cockpit_web_server_get_flags (server) & COCKPIT_WEB_SERVER_FOR_TLS_PROXY);
    }

  return TRUE;
}

gboolean
cockpit_handler_external (CockpitWebServer *server,
                          const gchar *original_path,
                          const gchar *path,
                          const gchar *method,
                          GIOStream *io_stream,
                          GHashTable *headers,
                          GByteArray *input,
                          CockpitHandlerData *ws)
{
  CockpitWebResponse *response = NULL;
  CockpitWebService *service = NULL;
  const gchar *segment = NULL;
  JsonObject *open = NULL;
  const gchar *query = NULL;
  CockpitCreds *creds;
  const gchar *expected;
  const gchar *upgrade;
  guchar *decoded;
  GBytes *bytes;
  gsize length;
  gsize seglen;

  /* The path must start with /cockpit+xxx/channel/csrftoken? or similar */
  if (path && path[0])
    segment = strchr (path + 1, '/');
  if (!segment)
    return FALSE;
  if (!g_str_has_prefix (segment, "/channel/"))
    return FALSE;
  segment += 9;

  /* Make sure we are authenticated, otherwise 404 */
  service = cockpit_auth_check_cookie (ws->auth, path, headers);
  if (!service)
    return FALSE;

  creds = cockpit_web_service_get_creds (service);
  g_return_val_if_fail (creds != NULL, FALSE);

  expected = cockpit_creds_get_csrf_token (creds);
  g_return_val_if_fail (expected != NULL, FALSE);

  /* The end of the token */
  query = strchr (segment, '?');
  if (query)
    {
      seglen = query - segment;
      query += 1;
    }
  else
    {
      seglen = strlen (segment);
      query = "";
    }

  /* No such path is valid */
  if (strlen (expected) != seglen || memcmp (expected, segment, seglen) != 0)
    {
      g_message ("invalid csrf token");
      return FALSE;
    }

  decoded = g_base64_decode (query, &length);
  if (decoded)
    {
      bytes = g_bytes_new_take (decoded, length);
      if (!cockpit_transport_parse_command (bytes, NULL, NULL, &open))
        {
          open = NULL;
          g_message ("invalid external channel query");
        }
      g_bytes_unref (bytes);
    }

  if (!open)
    {
      response = cockpit_web_response_new (io_stream, original_path, path, NULL, headers,
                                           (cockpit_web_server_get_flags (server) & COCKPIT_WEB_SERVER_FOR_TLS_PROXY) ?
                                             COCKPIT_WEB_RESPONSE_FOR_TLS_PROXY : COCKPIT_WEB_RESPONSE_NONE);

      cockpit_web_response_error (response, 400, NULL, NULL);
      g_object_unref (response);
    }
  else
    {
      upgrade = g_hash_table_lookup (headers, "Upgrade");
      if (upgrade && g_ascii_strcasecmp (upgrade, "websocket") == 0)
        {
          cockpit_channel_socket_open (service, open, original_path, path, io_stream, headers, input,
                                       cockpit_web_server_get_flags (server) & COCKPIT_WEB_SERVER_FOR_TLS_PROXY);
        }
      else
        {
          response = cockpit_web_response_new (io_stream, original_path, path, NULL, headers,
                                               (cockpit_web_server_get_flags (server) & COCKPIT_WEB_SERVER_FOR_TLS_PROXY) ?
                                                 COCKPIT_WEB_RESPONSE_FOR_TLS_PROXY : COCKPIT_WEB_RESPONSE_NONE);
          cockpit_web_response_set_method (response, method);
          cockpit_channel_response_open (service, headers, response, open);
          g_object_unref (response);
        }
      json_object_unref (open);
    }

  g_object_unref (service);

  return TRUE;
}


static void
add_oauth_to_environment (JsonObject *environment)
{
  static const gchar *url;
  JsonObject *object;

  url = cockpit_conf_string ("OAuth", "URL");

  if (url)
    {
      object = json_object_new ();
      json_object_set_string_member (object, "URL", url);
      json_object_set_string_member (object, "ErrorParam",
                                     cockpit_conf_string ("oauth", "ErrorParam"));
      json_object_set_string_member (object, "TokenParam",
                                     cockpit_conf_string ("oauth", "TokenParam"));
      json_object_set_object_member (environment, "OAuth", object);
  }
}

static void
add_page_to_environment (JsonObject *object,
                         gboolean    is_cockpit_client)
{
  static gint page_login_to = -1;
  gboolean require_host = FALSE;
  JsonObject *page;
  const gchar *value;

  page = json_object_new ();

  value = cockpit_conf_string ("WebService", "LoginTitle");
  if (value)
    json_object_set_string_member (page, "title", value);

  if (page_login_to < 0)
    {
      page_login_to = cockpit_conf_bool ("WebService", "LoginTo",
                                         g_file_test (cockpit_ws_ssh_program,
                                                      G_FILE_TEST_IS_EXECUTABLE));
    }

  require_host = is_cockpit_client || cockpit_conf_bool ("WebService", "RequireHost", FALSE);

  json_object_set_boolean_member (page, "connect", page_login_to);
  json_object_set_boolean_member (page, "require_host", require_host);
  json_object_set_object_member (object, "page", page);
}

static GBytes *
build_environment (GHashTable *os_release)
{
  /*
   * We don't include entirety of os-release into the
   * environment for the login.html page. There could
   * be unexpected things in here.
   *
   * However since we are displaying branding based on
   * the OS name variant flavor and version, including
   * the corresponding information is not a leak.
   */
  static const gchar *release_fields[] = {
    "NAME", "ID", "PRETTY_NAME", "VARIANT", "VARIANT_ID", "CPE_NAME", "ID_LIKE", "DOCUMENTATION_URL"
  };

  static const gchar *prefix = "\n    <script>\nvar environment = ";
  static const gchar *suffix = ";\n    </script>";

  GByteArray *buffer;
  GBytes *bytes;
  JsonObject *object;
  const gchar *value;
  gchar *hostname;
  JsonObject *osr;
  gint i;

  object = json_object_new ();

  gboolean is_cockpit_client = cockpit_conf_bool ("WebService", "X-For-CockpitClient", FALSE);
  json_object_set_boolean_member (object, "is_cockpit_client", is_cockpit_client);

  add_page_to_environment (object, is_cockpit_client);

  hostname = g_malloc0 (HOST_NAME_MAX + 1);
  gethostname (hostname, HOST_NAME_MAX);
  hostname[HOST_NAME_MAX] = '\0';
  json_object_set_string_member (object, "hostname", hostname);
  g_free (hostname);

  if (os_release)
    {
      osr = json_object_new ();
      for (i = 0; i < G_N_ELEMENTS (release_fields); i++)
        {
          value = g_hash_table_lookup (os_release, release_fields[i]);
          if (value)
            json_object_set_string_member (osr, release_fields[i], value);
        }
      json_object_set_object_member (object, "os-release", osr);
    }

  add_oauth_to_environment (object);

  g_autofree gchar *ca_path = locate_selfsign_ca ();
  if (ca_path)
    json_object_set_string_member (object, "CACertUrl", "/ca.cer");

  g_autofree gchar *contents = NULL;
  g_autoptr(GError) error = NULL;
  gsize len;

  const gchar *banner = cockpit_conf_string ("Session", "Banner");
  if (banner)
    {
      // TODO: parse macros (see `man agetty` for possible macros)
      g_file_get_contents (banner, &contents, &len, &error);
      if (error)
        g_message ("error loading contents of banner: %s", error->message);
      else
        json_object_set_string_member (object, "banner", contents);
    }

  bytes = cockpit_json_write_bytes (object);
  json_object_unref (object);

  buffer = g_bytes_unref_to_array (bytes);
  g_byte_array_prepend (buffer, (const guint8 *)prefix, strlen (prefix));
  g_byte_array_append (buffer, (const guint8 *)suffix, strlen (suffix));
  return g_byte_array_free_to_bytes (buffer);
}

static void
send_login_html (CockpitWebResponse *response,
                 CockpitHandlerData *ws,
                 const gchar *path,
                 GHashTable *headers)
{
  static const gchar *marker = "<meta insert_dynamic_content_here>";
  static const gchar *po_marker = "/*insert_translations_here*/";

  CockpitWebFilter *filter;
  GBytes *environment;
  GError *error = NULL;
  GBytes *bytes;

  GBytes *url_bytes = NULL;
  CockpitWebFilter *filter2 = NULL;
  const gchar *url_root = NULL;
  const gchar *accept = NULL;
  gchar *content_security_policy = NULL;
  gchar *cookie_line = NULL;
  gchar *base;

  gchar *language = NULL;
  gchar **languages = NULL;
  GBytes *po_bytes;
  CockpitWebFilter *filter3 = NULL;

  environment = build_environment (ws->os_release);
  filter = cockpit_web_inject_new (marker, environment, 1);
  g_bytes_unref (environment);
  cockpit_web_response_add_filter (response, filter);
  g_object_unref (filter);

  url_root = cockpit_web_response_get_url_root (response);
  if (url_root)
    base = g_strdup_printf ("<base href=\"%s/\">", url_root);
  else
    base = g_strdup ("<base href=\"/\">");

  url_bytes = g_bytes_new_take (base, strlen(base));
  filter2 = cockpit_web_inject_new (marker, url_bytes, 1);
  g_bytes_unref (url_bytes);
  cockpit_web_response_add_filter (response, filter2);
  g_object_unref (filter2);

  cockpit_web_response_set_cache_type (response, COCKPIT_WEB_RESPONSE_NO_CACHE);

  if (ws->login_po_js)
    {
      language = cockpit_web_server_parse_cookie (headers, "CockpitLang");
      if (!language)
        {
          accept = g_hash_table_lookup (headers, "Accept-Language");
          languages = cockpit_web_server_parse_accept_list (accept, NULL);
          language = languages[0];
        }

      po_bytes = cockpit_web_response_negotiation (ws->login_po_js, NULL, language, NULL, &error);
      if (error)
        {
          g_message ("%s", error->message);
          g_clear_error (&error);
        }
      else if (po_bytes)
        {
          filter3 = cockpit_web_inject_new (po_marker, po_bytes, 1);
          g_bytes_unref (po_bytes);
          cockpit_web_response_add_filter (response, filter3);
          g_object_unref (filter3);
        }
    }

  bytes = cockpit_web_response_negotiation (ws->login_html, NULL, NULL, NULL, &error);
  if (error)
    {
      g_message ("%s", error->message);
      cockpit_web_response_error (response, 500, NULL, NULL);
      g_error_free (error);
    }
  else if (!bytes)
    {
      cockpit_web_response_error (response, 404, NULL, NULL);
    }
  else
    {
      /* The login Content-Security-Policy allows the page to have inline <script> and <style> tags. */
      gboolean secure = g_strcmp0 (cockpit_web_response_get_protocol (response, headers), "https") == 0;
      cookie_line = cockpit_auth_empty_cookie_value (path, secure);
      content_security_policy = cockpit_web_response_security_policy ("default-src 'self' 'unsafe-inline'",
                                                                      cockpit_web_response_get_origin (response));

      cockpit_web_response_headers (response, 200, "OK", -1,
                                    "Content-Type", "text/html",
                                    "Content-Security-Policy", content_security_policy,
                                    "Set-Cookie", cookie_line,
                                    NULL);
      if (cockpit_web_response_queue (response, bytes))
        cockpit_web_response_complete (response);

      g_bytes_unref (bytes);
    }

  g_free (cookie_line);
  g_free (content_security_policy);
  g_strfreev (languages);
}

static void
send_login_response (CockpitWebResponse *response,
                     JsonObject *object,
                     GHashTable *headers)
{
  GBytes *content;

  content = cockpit_json_write_bytes (object);

  g_hash_table_replace (headers, g_strdup ("Content-Type"), g_strdup ("application/json"));
  cockpit_web_response_content (response, headers, content, NULL);
  g_bytes_unref (content);
}

static void
on_login_complete (GObject *object,
                   GAsyncResult *result,
                   gpointer user_data)
{
  CockpitWebResponse *response = user_data;
  GError *error = NULL;
  JsonObject *response_data = NULL;
  GHashTable *headers;
  GIOStream *io_stream;
  GBytes *content;

  io_stream = cockpit_web_response_get_stream (response);

  headers = cockpit_web_server_new_table ();
  response_data = cockpit_auth_login_finish (COCKPIT_AUTH (object), result,
                                             io_stream, headers, &error);

  /* Never cache a login response */
  cockpit_web_response_set_cache_type (response, COCKPIT_WEB_RESPONSE_NO_CACHE);
  if (error)
    {
      if (response_data)
        {
          g_hash_table_insert (headers, g_strdup ("Content-Type"), g_strdup ("application/json"));
          content = cockpit_json_write_bytes (response_data);
          cockpit_web_response_headers_full (response, 401, "Authentication required", -1, headers);
          cockpit_web_response_queue (response, content);
          cockpit_web_response_complete (response);
          g_bytes_unref (content);
        }
      else
        {
          cockpit_web_response_gerror (response, headers, error);
        }
      g_error_free (error);
    }
  else
    {
      send_login_response (response, response_data, headers);
    }

  if (response_data)
    json_object_unref (response_data);

  g_hash_table_unref (headers);
  g_object_unref (response);
}

static void
handle_login (CockpitHandlerData *data,
              CockpitWebService *service,
              const gchar *path,
              GHashTable *headers,
              CockpitWebResponse *response)
{
  GHashTable *out_headers;
  GIOStream *io_stream;
  CockpitCreds *creds;
  JsonObject *creds_json = NULL;

  if (service)
    {
      out_headers = cockpit_web_server_new_table ();
      creds = cockpit_web_service_get_creds (service);
      creds_json = cockpit_creds_to_json (creds);
      send_login_response (response, creds_json, out_headers);
      g_hash_table_unref (out_headers);
      json_object_unref (creds_json);
      return;
    }

  io_stream = cockpit_web_response_get_stream (response);
  cockpit_auth_login_async (data->auth, path,io_stream, headers,
                            on_login_complete, g_object_ref (response));
}

static void
handle_resource (CockpitHandlerData *data,
                 CockpitWebService *service,
                 const gchar *path,
                 GHashTable *headers,
                 CockpitWebResponse *response)
{
  gchar *where;

  where = cockpit_web_response_pop_path (response);
  if (where && (where[0] == '@' || where[0] == '$') && where[1] != '\0')
    {
      if (service)
        {
          cockpit_channel_response_serve (service, headers, response, where,
                                          cockpit_web_response_get_path (response));
        }
      else if (g_str_has_suffix (path, ".html"))
        {
          send_login_html (response, data, path, headers);
        }
      else
        {
          cockpit_web_response_error (response, 401, NULL, NULL);
        }
    }
  else
    {
      cockpit_web_response_error (response, 404, NULL, NULL);
    }

  g_free (where);
}

static void
handle_shell (CockpitHandlerData *data,
              CockpitWebService *service,
              const gchar *path,
              GHashTable *headers,
              CockpitWebResponse *response)
{
  gboolean valid;
  const gchar *shell_path;

  /* Check if a valid path for a shell to be served at */
  valid = g_str_equal (path, "/") ||
          g_str_has_prefix (path, "/@") ||
          g_str_has_prefix (path, "/=") ||
          strspn (path + 1, COCKPIT_RESOURCE_PACKAGE_VALID) == strcspn (path + 1, "/");

  if (g_str_has_prefix (path, "/=/") ||
      g_str_has_prefix (path, "/@/") ||
      g_str_has_prefix (path, "//"))
    {
      valid = FALSE;
    }

  if (!valid)
    {
      cockpit_web_response_error (response, 404, NULL, NULL);
    }
  else if (service)
    {
      shell_path = cockpit_conf_string ("WebService", "Shell");
      cockpit_channel_response_serve (service, headers, response, NULL,
                                      shell_path ? shell_path : cockpit_ws_shell_component);
      cockpit_web_response_set_cache_type (response, COCKPIT_WEB_RESPONSE_NO_CACHE);
    }
  else
    {
      send_login_html (response, data, path, headers);
    }
}

gboolean
cockpit_handler_default (CockpitWebServer *server,
                         const gchar *path,
                         GHashTable *headers,
                         CockpitWebResponse *response,
                         CockpitHandlerData *data)
{
  CockpitWebService *service;
  const gchar *remainder = NULL;
  gboolean resource;

  path = cockpit_web_response_get_path (response);
  g_return_val_if_fail (path != NULL, FALSE);

  resource = g_str_has_prefix (path, "/cockpit/") ||
             g_str_has_prefix (path, "/cockpit+") ||
             g_str_equal (path, "/cockpit");

  // Check for auth
  service = cockpit_auth_check_cookie (data->auth, path, headers);

  /* Stuff in /cockpit or /cockpit+xxx */
  if (resource)
    {
      g_assert (cockpit_web_response_skip_path (response));
      remainder = cockpit_web_response_get_path (response);

      if (!remainder)
        {
          cockpit_web_response_error (response, 404, NULL, NULL);
          return TRUE;
        }
      else if (g_str_has_prefix (remainder, "/static/"))
        {
          cockpit_branding_serve (service, response, path, remainder + 8,
                                  data->os_release, data->branding_roots);
          return TRUE;
        }
    }

  if (resource)
    {
      if (g_str_equal (remainder, "/login"))
        {
          handle_login (data, service, path, headers, response);
        }
      else
        {
          handle_resource (data, service, path, headers, response);
        }
    }
  else
    {
      handle_shell (data, service, path, headers, response);
    }

  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

gboolean
cockpit_handler_root (CockpitWebServer *server,
                      const gchar *path,
                      GHashTable *headers,
                      CockpitWebResponse *response,
                      CockpitHandlerData *ws)
{
  /* Don't cache forever */
  cockpit_web_response_file (response, path, ws->branding_roots);
  return TRUE;
}

gboolean
cockpit_handler_ping (CockpitWebServer *server,
                      const gchar *path,
                      GHashTable *headers,
                      CockpitWebResponse *response,
                      CockpitHandlerData *ws)
{
  GHashTable *out_headers;
  const gchar *body;
  GBytes *content;

  out_headers = cockpit_web_server_new_table ();

  /*
   * The /ping request has unrestricted CORS enabled on it. This allows javascript
   * in the browser on embedding websites to check if Cockpit is available. These
   * websites could do this in another way (such as loading an image from Cockpit)
   * but this does it in the correct manner.
   *
   * See: http://www.w3.org/TR/cors/
   */
  g_hash_table_insert (out_headers, g_strdup ("Access-Control-Allow-Origin"), g_strdup ("*"));

  g_hash_table_insert (out_headers, g_strdup ("Content-Type"), g_strdup ("application/json"));
  body ="{ \"service\": \"cockpit\" }";
  content = g_bytes_new_static (body, strlen (body));

  cockpit_web_response_content (response, out_headers, content, NULL);

  g_bytes_unref (content);
  g_hash_table_unref (out_headers);

  return TRUE;
}

gboolean
cockpit_handler_ca_cert (CockpitWebServer *server,
                         const gchar *path,
                         GHashTable *headers,
                         CockpitWebResponse *response,
                         CockpitHandlerData *ws)
{
  g_autofree gchar *ca_path = NULL;

  ca_path = locate_selfsign_ca ();
  if (ca_path == NULL) {
    cockpit_web_response_error (response, 404, NULL, "CA certificate not found");
    return TRUE;
  }

  const gchar *root_dir[] = { "/", NULL };
  cockpit_web_response_file (response, ca_path, root_dir);
  return TRUE;
}
