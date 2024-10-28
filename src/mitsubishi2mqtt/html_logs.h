#ifndef WEB_FILES_H
#define WEB_FILES_H

const char LOGS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang='fr'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Logs - Easydan Controller</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 20px;
            background-color: #f0f0f0;
        }
        h1 {
            color: #333;
            text-align: center;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            background: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }

        #logWindow {
            background-color: #1e1e1e;
            color: #00ff00;
            font-family: monospace;
            padding: 15px;
            border-radius: 5px;
            height: 400px;
            overflow-y: auto;
            margin-bottom: 20px;
            box-shadow: 0 2px 5px rgba(0,0,0,0.2);
        }

        .log-entry {
            margin: 5px 0;
            border-bottom: 1px solid #333;
            padding-bottom: 5px;
        }

        .timestamp {
            color: #888;
            margin-right: 10px;
        }

        .controls {
            margin-bottom: 20px;
        }

        button {
            background-color: #4CAF50;
            color: white;
            padding: 10px 20px;
            border: none;
            border-radius: 4px;
            cursor: pointer;
            margin-right: 10px;
        }

        button:hover {
            background-color: #45a049;
        }

        .status {
            padding: 10px;
            border-radius: 4px;
            margin-bottom: 10px;
            margin-top: 10px;
        }

        .success {
            background-color: #dff0d8;
            color: #3c763d;
        }

        .error {
            background-color: #f2dede;
            color: #a94442;
        }

        .back-button {
            background-color: #666;
            color: white;
            padding: 10px 20px;
            border: none;
            border-radius: 4px;
            cursor: pointer;
            text-decoration: none;
            font-size: 16px;
            display: inline-block;
            margin-bottom: 20px;
        }

        .back-button:hover {
            background-color: #555;
        }

        .command-buttons {
            display: flex;
            flex-wrap: wrap;
            gap: 10px;
            margin-bottom: 20px;
        }

        .command-buttons button {
            flex: 1;
            min-width: 120px;
            background-color: #2196F3;
        }

        .command-buttons button:hover {
            background-color: #1976D2;
        }

        .manual-command {
            display: flex;
            gap: 10px;
            margin-bottom: 20px;
        }

        .manual-command input {
            flex: 1;
            min-width: 120px;
            padding: 8px;
            border: 1px solid #ddd;
            border-radius: 4px;
            font-size: 16px;
        }

        .manual-command button {
            background-color: #2196F3;
            min-width: 100px;
        }
    </style>
</head>
<body>
    <div class='container'>
        <h1>Easydan Controller</h1>
        
        <!-- Nouveau bloc Commandes -->
        <h2>Commandes</h2>
        <div class='controls command-buttons'>
            <button onclick='sendCommand("connection")'>Connexion</button>
            <button onclick='sendCommand("mode")'>Mode</button>
        </div>

        <!-- Nouvelle section pour la commande manuelle -->
        <h3>Commande manuelle</h3>
        <div class='manual-command'>
            <input type='text' id='messageToSendInput' placeholder='message à envoyer'>
            <button onclick='sendManualCommand()'>Envoyer</button>
        </div>

        <h2>Logs</h2>
        <div id='connectionStatus' class='status error'>Déconnecté</div>
        
        <div class='controls'>
            <button onclick='clearLogs()'>Effacer les logs</button>
            <button onclick='reconnectWebSocket()'>Reconnecter</button>
        </div>

        <div id='logWindow'></div>

        <h2>Mise à jour du firmware</h2>
        <form id='upload_form' enctype='multipart/form-data'>
            <input type='file' name='update' id='file' accept='.bin'>
            <button type='submit' class='button'>Update Firmware</button>
        </form>
        <div class='progress-bar'>
            <div class='progress' id='progress'></div>
        </div>
        <div id='status' class="status"></div>
    </div>

    <script>
        const form = document.getElementById('upload_form');
        const progress = document.getElementById('progress');
        const status = document.getElementById('status');
        const fileInput = document.getElementById('file');
        const submitButton = form.querySelector('button[type=\'submit\']');

        form.addEventListener('submit', async (e) => {
            e.preventDefault();
            
            const file = fileInput.files[0];
            if (!file) {
                showStatus('Please select a file', 'error');
                return;
            }
            
            if (!file.name.endsWith('.bin')) {
                showStatus('Please select a .bin file', 'error');
                return;
            }

            submitButton.disabled = true;
            const formData = new FormData();
            formData.append('update', file);

            try {
                const response = await fetch('/update', {
                    method: 'POST',
                    body: formData
                });

                if (response.ok) {
                    showStatus('Update successful! Device will restart...', 'success');
                    setTimeout(() => {
                        window.location.reload();
                    }, 5000);
                } else {
                    const text = await response.text();
                    showStatus(`Update failed: ${text}`, 'error');
                }
            } catch (error) {
                showStatus(`Error: ${error.message}`, 'error');
            } finally {
                submitButton.disabled = false;
            }
        });

        function showStatus(message, type) {
            status.textContent = message;
            status.className = 'status ' + type;
        }

        // Gérer l'affichage de la progression
        if (window.XMLHttpRequest) {
            const xhr = new XMLHttpRequest();
            xhr.upload.addEventListener('progress', (evt) => {
                if (evt.lengthComputable) {
                    const percentComplete = (evt.loaded / evt.total) * 100;
                    progress.style.width = percentComplete + '%';
                }
            }, false);
        }

        function sendCommand(command, messageToSend=null) {
            if (ws && ws.readyState === WebSocket.OPEN) {
                const payload = JSON.stringify({ command, messageToSend });
                ws.send(payload);
                console.log('Command sent:', command);
                console.log('Message sent:', messageToSend);
            } else {
                console.error('WebSocket is not connected');
                showStatus('WebSocket non connecté', 'error');
            }
        }

        function sendManualCommand() {
            const messageToSendInput = document.getElementById('messageToSendInput');
            const messageToSend = messageToSendInput.value.trim();
            
            if (messageToSend) {
                sendCommand('manual-command', messageToSend);
                messageToSendInput.value = ''; // Vide le champ après envoi
            }
        }

        // Ajouter un écouteur pour la touche Enter
        document.getElementById('messageToSendInput').addEventListener('keypress', function(e) {
            if (e.key === 'Enter') {
                sendManualCommand();
            }
        });
    </script>

    <script>
        let ws;
        const logWindow = document.getElementById('logWindow');
        const statusDiv = document.getElementById('connectionStatus');

        function connectWebSocket() {
            // Remplacez avec l'adresse IP de votre ESP32
            ws = new WebSocket('ws://10.0.0.87:81');

            ws.onopen = function() {
                statusDiv.textContent = 'Connecté';
                statusDiv.className = 'status success';
            };

            ws.onclose = function() {
                statusDiv.textContent = 'Déconnecté';
                statusDiv.className = 'status error';
                // Tentative de reconnexion après 5 secondes
                setTimeout(reconnectWebSocket, 5000);
            };
            
            ws.onerror = function(error) {
                console.error('Erreur WebSocket:', error);
                statusDiv.textContent = 'Erreur de connexion';
            };

            ws.onmessage = function(event) {
                const timestamp = new Date().toLocaleTimeString();
                const logEntry = document.createElement('div');
                logEntry.className = 'log-entry';
                logEntry.innerHTML = `<span class='timestamp'>[${timestamp}]</span> ${event.data}`;
                logWindow.appendChild(logEntry);
                logWindow.scrollTop = logWindow.scrollHeight;
            };
        }

        function clearLogs() {
            logWindow.innerHTML = '';
        }

        function reconnectWebSocket() {
            if (ws) {
                ws.close();
            }
            connectWebSocket();
        }

        // Connexion initiale
        connectWebSocket();
    </script>
</body>
</html>
)rawliteral";
#endif
