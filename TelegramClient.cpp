#include "TelegramClient.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "UrlEncode.h"
#include "arduino_secrets.h"

namespace {
constexpr const char *TELEGRAM_HOST = "api.telegram.org";
constexpr uint16_t TELEGRAM_HTTPS_PORT = 443;

bool sendMultipartAudio(const RecordedAudio &audio, const String &caption) {
  if (!audio.ok || audio.wav == nullptr || audio.wavSize == 0) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);

  if (!client.connect(TELEGRAM_HOST, TELEGRAM_HTTPS_PORT)) {
    return false;
  }

  const String boundary = "----JacobSmartSpeakerBoundary";
  String chatPart = "--" + boundary + "\r\n";
  chatPart += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
  chatPart += TELEGRAM_CHAT_ID;
  chatPart += "\r\n";

  String captionPart;
  if (caption.length() > 0) {
    captionPart = "--" + boundary + "\r\n";
    captionPart += "Content-Disposition: form-data; name=\"caption\"\r\n\r\n";
    captionPart += caption;
    captionPart += "\r\n";
  }

  String audioHeader = "--" + boundary + "\r\n";
  audioHeader += "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n";
  audioHeader += "Content-Type: audio/wav\r\n\r\n";

  String closing = "\r\n--" + boundary + "--\r\n";
  size_t contentLength = chatPart.length() + captionPart.length() + audioHeader.length() + audio.wavSize + closing.length();

  String path = "/bot";
  path += TELEGRAM_BOT_TOKEN;
  path += "/sendAudio";

  client.print("POST ");
  client.print(path);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(TELEGRAM_HOST);
  client.println("Connection: close");
  client.print("Content-Type: multipart/form-data; boundary=");
  client.println(boundary);
  client.print("Content-Length: ");
  client.println(contentLength);
  client.println();

  client.print(chatPart);
  if (captionPart.length() > 0) {
    client.print(captionPart);
  }
  client.print(audioHeader);
  client.write(audio.wav, audio.wavSize);
  client.print(closing);

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  client.stop();

  return statusLine.startsWith("HTTP/1.1 2") || statusLine.startsWith("HTTP/1.0 2");
}
}

bool sendTelegramMessage(const String &message) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += TELEGRAM_BOT_TOKEN;
  url += "/sendMessage";

  if (!http.begin(client, url)) {
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "chat_id=";
  body += urlEncode(TELEGRAM_CHAT_ID);
  body += "&text=";
  body += urlEncode(message);

  int status = http.POST(body);
  http.end();

  return status >= 200 && status < 300;
}

bool sendTelegramAudio(const RecordedAudio &audio, const String &caption) {
  return sendMultipartAudio(audio, caption);
}

bool getTelegramUpdates(int64_t offset, String &response) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += TELEGRAM_BOT_TOKEN;
  url += "/getUpdates?timeout=0";
  if (offset >= 0) {
    url += "&offset=";
    url += String(static_cast<long long>(offset));
  }

  if (!http.begin(client, url)) {
    return false;
  }

  int status = http.GET();
  response = status >= 200 && status < 300 ? http.getString() : "";
  http.end();
  return status >= 200 && status < 300;
}

bool getTelegramFilePath(const String &fileId, String &filePath) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://api.telegram.org/bot";
  url += TELEGRAM_BOT_TOKEN;
  url += "/getFile?file_id=";
  url += fileId;

  if (!http.begin(client, url)) {
    return false;
  }

  int status = http.GET();
  String response = status >= 200 && status < 300 ? http.getString() : "";
  http.end();
  if (status < 200 || status >= 300) {
    return false;
  }

  int key = response.indexOf("\"file_path\":\"");
  if (key < 0) {
    return false;
  }
  int start = key + 13;
  int end = response.indexOf('"', start);
  if (end < 0) {
    return false;
  }

  filePath = response.substring(start, end);
  filePath.replace("\\/", "/");
  return filePath.length() > 0;
}

bool downloadTelegramFile(const String &filePath, uint8_t **buffer, size_t &size, size_t maxBytes) {
  *buffer = nullptr;
  size = 0;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://api.telegram.org/file/bot";
  url += TELEGRAM_BOT_TOKEN;
  url += "/";
  url += filePath;

  if (!http.begin(client, url)) {
    return false;
  }

  int status = http.GET();
  if (status < 200 || status >= 300) {
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0 || static_cast<size_t>(contentLength) > maxBytes) {
    http.end();
    return false;
  }

  uint8_t *data = static_cast<uint8_t *>(ps_malloc(contentLength));
  if (data == nullptr) {
    data = static_cast<uint8_t *>(malloc(contentLength));
  }
  if (data == nullptr) {
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t received = 0;
  uint32_t deadline = millis() + 20000;
  while (received < static_cast<size_t>(contentLength) && millis() < deadline) {
    int available = stream->available();
    if (available <= 0) {
      delay(10);
      continue;
    }
    size_t toRead = min<size_t>(available, static_cast<size_t>(contentLength) - received);
    int read = stream->readBytes(data + received, toRead);
    if (read > 0) {
      received += read;
      deadline = millis() + 20000;
    }
  }

  http.end();
  if (received != static_cast<size_t>(contentLength)) {
    free(data);
    return false;
  }

  *buffer = data;
  size = received;
  return true;
}
