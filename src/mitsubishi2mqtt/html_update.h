const char UPDATE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Mise à jour - Easydan Controller</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 20px;
            background: #f0f0f0;
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
        .progress-bar {
            width: 100%;
            background-color: #f0f0f0;
            padding: 3px;
            border-radius: 3px;
            box-shadow: inset 0 1px 3px rgba(0,0,0,.2);
            margin: 20px 0;
        }
        .progress {
            width: 0%;
            height: 20px;
            background-color: #4CAF50;
            border-radius: 3px;
            transition: width 500ms ease-in-out;
        }
        .button {
            background-color: #4CAF50;
            border: none;
            color: white;
            padding: 15px 32px;
            text-align: center;
            text-decoration: none;
            display: inline-block;
            font-size: 16px;
            margin: 4px 2px;
            cursor: pointer;
            border-radius: 4px;
        }
        .button:disabled {
            background-color: #cccccc;
            cursor: not-allowed;
        }
        #status {
            margin: 20px 0;
            padding: 10px;
            border-radius: 4px;
        }
        .error {
            background-color: #ffebee;
            color: #c62828;
        }
        .success {
            background-color: #e8f5e9;
            color: #2e7d32;
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
    </style>
</head>
<body>
    <div class='container'>
        <h1>Easydan Controller</h1>
        <a href="/" class="back-button">← Retour</a>
        <h2>Mise à jour du firmware</h2>
        <form id='upload_form' enctype='multipart/form-data'>
            <input type='file' name='update' id='file' accept='.bin'>
            <button type='submit' class='button'>Update Firmware</button>
        </form>
        <div class='progress-bar'>
            <div class='progress' id='progress'></div>
        </div>
        <div id='status' class="connected"></div>
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
            status.className = type;
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
    </script>
</body>
</html>
)rawliteral";
