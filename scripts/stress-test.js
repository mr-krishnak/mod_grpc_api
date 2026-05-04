const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');
const path = require('path');

const PROTO_PATH = path.join(__dirname, 'freeswitch_api.proto');
const packageDefinition = protoLoader.loadSync(PROTO_PATH, {
    keepCase: true, longs: String, enums: String, defaults: true, oneofs: true
});
const fsgrpc = grpc.loadPackageDefinition(packageDefinition).fsgrpc;

// --- CONFIGURATION ---
const TARGET_IP = 'localhost:50051';
const CONCURRENCY = 100;    // Max parallel requests at any moment
const TOTAL_REQUESTS = 100000; 

const client = new fsgrpc.FreeSwitchApi(TARGET_IP, grpc.credentials.createInsecure());

let successCount = 0;
let errorCount = 0;
let completedCount = 0;

async function runWorker() {
    while (completedCount < TOTAL_REQUESTS) {
        // Increment global counter to "claim" a request
        const currentReqId = completedCount++;
        if (currentReqId >= TOTAL_REQUESTS) break;

        await new Promise((resolve) => {
            client.Execute({ command: 'show', arguments: 'status' }, (error, response) => {
                // console.log("success:", response.success);
                // console.log("message:", response.message);
                
                if (!error && response.success) {
                    successCount++;
                } else {
                    // console.error("Error:", error ? error.message : response.message);
                    errorCount++;
                }
                resolve();
            });
        });
    }
}

async function runTest() {
    console.log(`🚀 Starting Stress Test: ${TOTAL_REQUESTS} requests`);
    console.log(`📡 Target: ${TARGET_IP} | Concurrency: ${CONCURRENCY}\n`);

    const startTime = Date.now();

    // Start 'CONCURRENCY' number of parallel worker loops
    const workers = [];
    for (let i = 0; i < CONCURRENCY; i++) {
        workers.push(runWorker());
    }

    // Wait for all worker loops to finish
    await Promise.all(workers);

    const endTime = Date.now();
    const durationSec = (endTime - startTime) / 1000;
    const tps = (successCount / durationSec).toFixed(2);

    console.log('--- 📊 Stress Test Results ---');
    console.log(`Total Time:      ${durationSec.toFixed(2)}s`);
    console.log(`Success:         ${successCount}`);
    console.log(`Failures:        ${errorCount}`);
    console.log(`Throughput:      ${tps} TPS`);
    console.log('------------------------------');
    
    process.exit(0);
}

runTest();