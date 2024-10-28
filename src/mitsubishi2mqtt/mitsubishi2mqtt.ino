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
  }
}

void loop()
{
  webSocket.loop();

  delay(10);
}
