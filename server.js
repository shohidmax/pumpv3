require('dotenv').config();
const WebSocket = require('ws');
const http = require('http');
const express = require('express');
const path = require('path');
const mongoose = require('mongoose');

const app = express();
const server = http.createServer(app);

// --- MongoDB Setup ---
const MONGODB_URI = process.env.MONGODB_URI || "mongodb://localhost:27017/motor_data";
mongoose.connect(MONGODB_URI)
    .then(() => console.log('MongoDB Connected'))
    .catch(err => console.error('MongoDB connection error:', err));

// Updated Schema as per user request
const LogSchema = new mongoose.Schema({
    macAddress: String,
    startTime: Date,
    endTime: Date,
    duration: String,
    bdDate: String,
    bdTime: String,
    createdAt: { type: Date, default: Date.now }
});
const MotorLog = mongoose.model('MotorLog4', LogSchema);

// --- Static Files ---
// Serve files from the sibling 'Dashboard' directory
app.use(express.static(path.join(__dirname, '../Dashboard')));

app.get('/status', (req, res) => {
    res.send('server is running');
});

// Explicit root handler as requested
app.get('/', (req, res) => {
    res.send('server is running ...');
});

const wss = new WebSocket.Server({ server });

let esp32Client = null;
const webClients = new Set();

// --- Motor State Tracking ---
let motorStartTime = null;
let lastMotorStatus = 'OFF';
const DEVICE_MAC = "68:FE:71:8A:85:30"; // Hardcoded MAC

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
            
            // Send current motor status if known
            if (lastMotorStatus !== 'OFF') {
                 // You might want to send the last known full payload here if you stored it
            }
        }
    }, 2000);

    ws.on('message', async (message) => {
        let data;
        try {
            data = JSON.parse(message);
        } catch (e) {
            // console.error('Invalid JSON received:', message.toString());
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
            // Handle Web Client Commands
            if (data.command === 'GET_LOG_PAGE') {
                const page = data.value || 0;
                const limit = 10;
                
                // Build Query
                let query = {};
                if (data.startDate && data.endDate) {
                    const start = new Date(data.startDate);
                    start.setHours(0,0,0,0);
                    
                    const end = new Date(data.endDate);
                    end.setHours(23,59,59,999);
                    
                    query.startTime = { $gte: start, $lte: end };
                    console.log(`Filtering logs from ${start} to ${end}`);
                }

                try {
                    const totalLogs = await MotorLog.countDocuments(query);
                    const totalPages = Math.ceil(totalLogs / limit);
                    const logs = await MotorLog.find(query)
                        .sort({ createdAt: -1 })
                        .skip(page * limit)
                        .limit(limit);
                    
                    const logStrings = logs.map(log => {
                        // Helper to format Date to BD Time
                        const formatBD = (date) => {
                            if (!date) return 'N/A';
                            const optionsDate = { timeZone: 'Asia/Dhaka', day: '2-digit', month: '2-digit', year: 'numeric' };
                            const optionsTime = { timeZone: 'Asia/Dhaka', hour: 'numeric', minute: 'numeric', second: 'numeric', hour12: true };
                            return new Intl.DateTimeFormat('en-GB', optionsDate).format(date) + ' ' + 
                                   new Intl.DateTimeFormat('en-US', optionsTime).format(date);
                        };

                        return JSON.stringify({
                            onTime: formatBD(log.startTime),
                            offTime: log.bdDate + ' ' + log.bdTime, // stored BD time is end time
                            duration: log.duration
                        });
                    });

                    ws.send(JSON.stringify({
                        type: 'logPageUpdate',
                        payload: {
                            motorLogs: logStrings,
                            currentPage: page,
                            totalPages: totalPages
                        }
                    }));
                } catch (err) {
                    console.error("Error fetching logs:", err);
                }
            } else if (data.command === 'CLEAR_LOGS') {
                try {
                    await MotorLog.deleteMany({});
                    console.log("All logs cleared.");
                    // Notify all web clients
                     webClients.forEach(client => {
                        if (client.readyState === WebSocket.OPEN) {
                             client.send(JSON.stringify({
                                 type: 'statusUpdate',
                                 payload: {
                                    lastAction: "Logs Cleared", 
                                    motorStatus: lastMotorStatus,
                                    systemMode: "Normal"
                                 }
                             }));
                        }
                    });
                } catch (err) {
                    console.error("Error clearing logs:", err);
                }
            } else {
                 // Forward other commands (RELAY, RESET, RESTART) to ESP32
                if (esp32Client && esp32Client.readyState === WebSocket.OPEN) {
                    console.log('Forwarding command to ESP32:', message.toString());
                    esp32Client.send(message.toString());
                }
            }

        } else if (data.type === 'statusUpdate' && ws === esp32Client) {
            // Handle ESP32 Status Updates
            const payload = data.payload;
            const currentMotorStatus = payload.motorStatus;

            // Logic to track duration and save log
            if (currentMotorStatus === 'ON' && lastMotorStatus === 'OFF') {
                motorStartTime = new Date();
                console.log("Motor Started at:", motorStartTime);
            } else if (currentMotorStatus === 'OFF' && lastMotorStatus === 'ON' && motorStartTime) {
                const motorStopTime = new Date();
                const durationMs = motorStopTime - motorStartTime;
                const durationSec = Math.floor(durationMs / 1000);
                
                // Format Duration
                const durationStr = `${Math.floor(durationSec / 60)}m ${durationSec % 60}s`;

                // Filter: Ignore logs less than 2 seconds (Switch bounce / noise)
                if (durationSec < 2) {
                     console.log(`Skipping Short Log (${durationSec}s)`);
                } else {
                    // Format BD Date/Time
                    const optionsDate = { timeZone: 'Asia/Dhaka', day: '2-digit', month: '2-digit', year: 'numeric' };
                    // ... (rest of code)
                    const optionsTime = { timeZone: 'Asia/Dhaka', hour: 'numeric', minute: 'numeric', second: 'numeric', hour12: true };
                    
                     // Enforcing DD/MM/YYYY for BD Date
                    try {
                       const bdDateParts = new Intl.DateTimeFormat('en-GB', optionsDate).formatToParts(motorStopTime);
                       const day = bdDateParts.find(p => p.type === 'day').value;
                       const month = bdDateParts.find(p => p.type === 'month').value;
                       const year = bdDateParts.find(p => p.type === 'year').value;
                       var bdDateFinal = `${day}/${month}/${year}`;
                    } catch(e) {
                       var bdDateFinal = motorStopTime.toLocaleDateString();
                    }
                    
                    const bdTimeFinal = motorStopTime.toLocaleTimeString('en-US', optionsTime);

                    console.log(`Motor Stopped. Duration: ${durationStr}`);

                    // Save to MongoDB
                    const newLog = new MotorLog({
                        macAddress: DEVICE_MAC,
                        startTime: motorStartTime,
                        endTime: motorStopTime,
                        duration: durationStr,
                        bdDate: bdDateFinal,
                        bdTime: bdTimeFinal
                    });
                    newLog.save().then(() => console.log("Log saved to DB")).catch(err => console.error(err));
                }

                motorStartTime = null; 
            }
            lastMotorStatus = currentMotorStatus;

            // Forward status to all web clients
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
            // Reset state? maybe not
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
    console.log('server is running');
});
