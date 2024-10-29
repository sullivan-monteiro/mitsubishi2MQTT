#include <WiFi.h>
#include <Update.h> // Ajout de cet en-tête pour OTA
#include "SPIFFS.h" // ESP32 SPIFFS

// #include "html_home.h"
#include "html_logs.h"
// #include "html_update.h"

#include <ESPAsyncWebServer.h>
AsyncWebServer server(80); // ESP32 web

#include <WebSocketsServer.h>
WebSocketsServer webSocket = WebSocketsServer(81);

#include <ArduinoJson.h>
#include <HeatPump.h> // SwiCago library: https://github.com/SwiCago/HeatPump

HeatPump hp;
// Paramètres WiFi
const char *ssid = "reseau-maison";
const char *password = "9vsKXMx$B9gKO1nY@zq15K";

bool clientConnected = false;

void setup()
{
  Serial.begin(115200);
  delay(1000);

  if (!SPIFFS.begin(true))
  {
    Serial.println("Erreur SPIFFS");
    return;
  }
  // Connexion au WiFi
  Serial.println("Connexion au WiFi...");
  WiFi.begin(ssid, password);

  // Attente de la connexion
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  // Affichage des informations de connexion
  Serial.println("");
  Serial.println("WiFi connecté!");
  Serial.print("Adresse IP: ");
  Serial.println(WiFi.localIP());

  server.on("/", [](AsyncWebServerRequest *request)
            { request->send(200, "text/html", LOGS_HTML); });
  // server.on("/", [](AsyncWebServerRequest *request)
  //           { request->send(200, "text/html", HOME_HTML); });
  // server.on("/logs", [](AsyncWebServerRequest *request)
  //           { request->send(200, "text/html", LOGS_HTML); });
  // server.onNotFound(handleNotFound);

  // server.on("/upgrade", HTTP_GET, [](AsyncWebServerRequest *request)
  //           { request->send(200, "text/html", UPDATE_HTML); });
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request)
            {
              // Cette fonction est appelée après la fin du téléchargement
              // La réponse est déjà envoyée dans handleUpdate
            },
            handleUpdate);

  server.begin();
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);

  hp.setPacketCallback(hpPacketDebug);
}

void hpPacketDebug(byte *packet, unsigned int length, const char *packetDirection)
{
  String message;
  for (unsigned int idx = 0; idx < length; idx++)
  {
    if (packet[idx] < 16)
    {
      message += "0"; // pad single hex digits with a 0
    }
    message += String(packet[idx], HEX) + " ";
  }

  const size_t bufferSize = JSON_OBJECT_SIZE(10);
  StaticJsonDocument<bufferSize> root;

  root[packetDirection] = message;
  String output;
  serializeJson(root, output);
  sendLog(output);
}

// Fonction pour convertir une chaîne en bytes hexadécimaux
byte *stringToHexBytes(const char *str, size_t *length)
{
  // Calcule la longueur de la chaîne
  size_t strLen = strlen(str);
  // Alloue la mémoire pour les bytes (la moitié de la longueur car 2 caractères hex = 1 byte)
  *length = strLen / 2;
  byte *bytes = new byte[*length];

  for (size_t i = 0; i < *length; i++)
  {
    // Prend deux caractères à la fois
    char highNibble = str[i * 2];
    char lowNibble = str[i * 2 + 1];

    // Convertit les caractères en valeurs numériques
    byte high = (highNibble >= 'A') ? (highNibble - 'A' + 10) : (highNibble - '0');
    byte low = (lowNibble >= 'A') ? (lowNibble - 'A' + 10) : (lowNibble - '0');

    // Combine les deux nibbles en un byte
    bytes[i] = (high << 4) | low;
  }

  return bytes;
}

// Gestionnaire de mise à jour
void handleUpdate(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
  static size_t contentLength = 0;   // Ajout de static
  static size_t currentLength = 0;   // Ajout de static
  static bool updateStarted = false; // Ajout de static
  static int lastProgress = 0;
  if (!index)
  {
    // Réinitialiser les variables au début
    contentLength = request->contentLength();
    currentLength = 0;
    updateStarted = false;

    Serial.println("Début de la mise à jour");
    Serial.printf("Nom du fichier: %s\n", filename.c_str());

    // Vérifier si c'est un fichier .bin
    if (!filename.endsWith(".bin"))
    {
      request->send(400, "text/plain", "Fichier non valide");
      return;
    }

    Serial.printf("Taille du fichier: %u bytes\n", contentLength);

    // Démarrer la mise à jour
    if (!Update.begin(contentLength))
    {
      Update.printError(Serial);
      request->send(400, "text/plain", "OTA impossible. Erreur #1");
      return;
    }

    updateStarted = true;
  }

  // Écriture des données
  if (updateStarted)
  {
    if (Update.write(data, len) != len)
    {
      Update.printError(Serial);
      request->send(400, "text/plain", "OTA impossible. Erreur #2");
      return;
    }

    currentLength += len;
    // Calcul du pourcentage
    int progress = (currentLength * 100) / contentLength;

    if (progress != lastProgress && progress % 5 == 0)
    {
      String progressMsg = "Upload progress: " + String(progress) + "%";
      sendLog(progressMsg);
      lastProgress = progress;
    }
  }

  // Fin de l'upload
  if (final)
  {
    if (!Update.end(true))
    {
      Update.printError(Serial);
      request->send(400, "text/plain", "OTA impossible. Erreur #3");
      return;
    }

    Serial.println("Mise à jour terminée");
    request->send(200, "text/plain", "OK");

    // Redémarrage différé
    delay(500);
    ESP.restart();
  }
}

void handleNotFound(AsyncWebServerRequest *request)
{

  AsyncWebServerResponse *response = request->beginResponse(302);
  response->addHeader("Location", "/");
  response->addHeader("Cache-Control", "no-cache");
  request->send(response);
  return;
}

void sendLog(const String &message)
{
  Serial.println(message);
  if (clientConnected)
  {
    webSocket.broadcastTXT(message.c_str()); // Conversion de String en const char*
  }
}

void onWebSocketEvent(uint8_t client_num, WStype_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case WStype_DISCONNECTED:
    Serial.printf("[%u] Déconnecté!\n", client_num);
    clientConnected = false;
    break;

  case WStype_CONNECTED:
    Serial.printf("[%u] Connecté!\n", client_num);
    clientConnected = true;
    break;

  case WStype_TEXT:
  {
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error)
    {
      Serial.print("Erreur parsing JSON: ");
      Serial.println(error.c_str());
      webSocket.sendTXT(client_num, "{\"error\": \"Format JSON invalide\"}");
      return;
    }

    const char *command = doc["command"];
    const char *messageToSend = doc["messageToSend"];

    if (command)
    {
      Serial.printf("Commande reçue: %s\n", command);

      if (strcmp(command, "connection") == 0)
      {
        // Traitement de la commande connection
        sendLog("Commande connection reçue");
        hp.connect(&Serial);
      }
      else if (strcmp(command, "mode") == 0)
      {
        // Traitement de la commande mode
        sendLog("Commande mode reçue");
      }
      else if (strcmp(command, "manual-command") == 0 && messageToSend)
      {
        size_t byteLength;
        byte *bytes = stringToHexBytes(messageToSend, &byteLength);

        // Utilisation des bytes
        hp.sendCustomPacket(bytes, byteLength);

        // Debug: afficher les bytes
        Serial.print("Bytes envoyés: ");
        for (size_t i = 0; i < byteLength; i++)
        {
          if (bytes[i] < 0x10)
            Serial.print("0");
          Serial.print(bytes[i], HEX);
          Serial.print(" ");
        }
        Serial.println();

        // Libérer la mémoire
        delete[] bytes;

        String logMessage = "Commande hex envoyée: " + String(messageToSend);
        sendLog(logMessage);
      }
    }
    break;
  }
  }
}

void loop()
{
  webSocket.loop();
  // hp.sync();
  delay(100);
}
