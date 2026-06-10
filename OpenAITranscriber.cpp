#include "OpenAITranscriber.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "arduino_secrets.h"

namespace {
String jsonStringValue(const String &json, const String &key) {
  String pattern = "\"" + key + "\":";
  int start = json.indexOf(pattern);
  if (start < 0) {
    return "";
  }

  start = json.indexOf('"', start + pattern.length());
  if (start < 0) {
    return "";
  }
  start++;

  String value;
  bool escaped = false;
  for (int i = start; i < json.length(); i++) {
    char c = json[i];
    if (escaped) {
      switch (c) {
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        default: value += c; break;
      }
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      break;
    } else {
      value += c;
    }
  }
  return value;
}
}

TranscriptionResult transcribeWithOpenAI(const RecordedAudio &audio) {
  TranscriptionResult result;
  if (!audio.ok || audio.wav == nullptr || audio.wavSize == 0) {
    result.error = "No audio to transcribe";
    return result;
  }

  const String boundary = "----JacobSmartSpeakerBoundary";
  String prefix;
  prefix += "--" + boundary + "\r\n";
  prefix += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
  prefix += OPENAI_TRANSCRIPTION_MODEL;
  prefix += "\r\n--" + boundary + "\r\n";
  prefix += "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n";
  prefix += "Content-Type: audio/wav\r\n\r\n";

  String suffix = "\r\n--" + boundary + "--\r\n";
  size_t bodySize = prefix.length() + audio.wavSize + suffix.length();

  uint8_t *body = static_cast<uint8_t *>(ps_malloc(bodySize));
  if (body == nullptr) {
    body = static_cast<uint8_t *>(malloc(bodySize));
  }
  if (body == nullptr) {
    result.error = "Unable to allocate request body";
    return result;
  }

  size_t offset = 0;
  memcpy(body + offset, prefix.c_str(), prefix.length());
  offset += prefix.length();
  memcpy(body + offset, audio.wav, audio.wavSize);
  offset += audio.wavSize;
  memcpy(body + offset, suffix.c_str(), suffix.length());

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, "https://api.openai.com/v1/audio/transcriptions")) {
    free(body);
    result.error = "HTTP begin failed";
    return result;
  }

  String auth = "Bearer ";
  auth += OPENAI_API_KEY;
  http.addHeader("Authorization", auth);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  result.httpStatus = http.POST(body, bodySize);
  free(body);

  String response = http.getString();
  http.end();

  if (result.httpStatus >= 200 && result.httpStatus < 300) {
    result.text = jsonStringValue(response, "text");
    result.ok = result.text.length() > 0;
    if (!result.ok) {
      result.error = "Transcription response did not include text";
    }
  } else {
    result.error = response;
  }

  return result;
}
