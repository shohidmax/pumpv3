const WebSocket = require('ws');
const http = require('http');
const express = require('express');
const path = require('path');

const app = express();
const server = http.createServer(app);

app.use(express.static(path.join(__dirname, 'public')));

const wss = new WebSocket.Server({ server });

let esp32Client = null;
const webClients = new Set();

wss.on('connection', (ws) => {
    console.log('A client connected. Waiting for identification...');

    ws.isIdentified = false;

    const identificationTimeout = setTimeout(() => {
        if (!ws.isIdentified) {
            console.log('Client did not identify. Assuming it is a web client.');
            webClients.add(ws);
            ws.isIdentified = true;
            const espStatus = (esp32Client && esp32Client.readyState === WebSocket.OPEN) ? 'online' : 'offline';
            ws.send(JSON.stringify({ type: 'espStatus', status: espStatus }));
        }
    }, 2000);

    ws.on('message', (message) => {
        let data;
        try {
            data = JSON.parse(message);
        } catch (e) {
            console.error('Invalid JSON received:', message.toString());
            return;
        }

        if (data.type === 'esp32-identify' && !ws.isIdentified) {
            clearTimeout(identificationTimeout);
            console.log('ESP32 client identified.');
            esp32Client = ws;
            ws.isIdentified = true;
            
            webClients.forEach(client => {
                if (client.readyState === WebSocket.OPEN) {
                    client.send(JSON.stringify({ type: 'espStatus', status: 'online' }));
                }
            });

        } else if (data.type === 'command' && ws !== esp32Client) {
            if (esp32Client && esp32Client.readyState === WebSocket.OPEN) {
                console.log('Forwarding command to ESP32:', message.toString());
                esp32Client.send(message.toString());
            }
        } else if ((data.type === 'statusUpdate' || data.type === 'allLogsUpdate' || data.type === 'logPageUpdate') && ws === esp32Client) {
            webClients.forEach(client => {
                if (client.readyState === WebSocket.OPEN) {
                    client.send(message.toString());
                }
            });
        }
    });

    ws.on('close', () => {
        clearTimeout(identificationTimeout);
        if (ws === esp32Client) {
            console.log('ESP32 client disconnected.');
            esp32Client = null;
            webClients.forEach(client => {
                if (client.readyState === WebSocket.OPEN) {
                    client.send(JSON.stringify({ type: 'espStatus', status: 'offline' }));
                }
            });
        } else {
            webClients.delete(ws);
            console.log('Web client disconnected.');
        }
    });

    ws.on('error', (error) => {
        console.error('WebSocket error:', error);
    });
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
    console.log(`Server is listening on port ${PORT}`);
});
