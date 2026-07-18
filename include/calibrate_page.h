#pragma once

// Guided wind-direction calibration page, served from the WiFiManager
// portal (see /calibrate, /calibrate/sample, /calibrate/save routes in
// runConfigPortal(), main.cpp). Works with any resistor-ladder/reed-switch
// wind vane (discrete voltage steps per direction) regardless of vendor —
// the user physically rotates the vane through each known position while
// this page captures the live ADC reading, building a calibration table
// without needing to know the sensor's internal resistor values.
//
// Self-contained (no CDN), so it also works while connected to the
// isolated WiFiManager AP portal.
static const char CALIBRATE_PAGE_HTML[] PROGMEM = R"CALPAGE(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Wind Direction Calibration</title>
<style>
  body{font-family:sans-serif;max-width:600px;margin:1rem auto;padding:0 1rem;line-height:1.5;}
  button{font-size:1rem;padding:0.5rem 1rem;margin:0.25rem 0.25rem 0.25rem 0;}
  table{border-collapse:collapse;margin:1rem 0;width:100%;}
  th,td{border:1px solid #ccc;padding:4px 8px;text-align:left;}
  #msg{margin-top:1rem;font-weight:bold;}
</style>
</head>
<body>
<h3>Wind Direction Calibration</h3>
<p>For resistor-ladder / reed-switch wind vanes only (discrete voltage steps per direction). Continuous-potentiometer sensors (e.g. Davis 6410) don't need this — leave "Wind direction sensor" at 0 on the parameter page instead.</p>

<div id="setup">
  <p>How many positions does your wind vane support?</p>
  <button onclick="start(8)">8 positions</button>
  <button onclick="start(16)">16 positions</button>
</div>

<div id="step" style="display:none">
  <p id="stepLabel"></p>
  <p>Live ADC reading: <strong id="liveAdc">-</strong></p>
  <button onclick="capture()">Capture</button>
</div>

<div id="done" style="display:none">
  <p>All positions captured:</p>
  <table id="resultTable"></table>
  <button onclick="save()">Save Calibration</button>
</div>

<div id="msg"></div>

<script>
var count = 0, idx = 0, positions = [];
var dirs = ['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'];

function compassLabel(deg) { return dirs[Math.round(deg / 22.5) % 16]; }

function start(n) {
  count = n; idx = 0; positions = [];
  document.getElementById('setup').style.display = 'none';
  document.getElementById('step').style.display = 'block';
  showStep();
}

function showStep() {
  var deg = Math.round(idx * 360 / count);
  document.getElementById('stepLabel').textContent =
    'Position ' + (idx + 1) + '/' + count + ': point the vane to ' + compassLabel(deg) + ' (' + deg + '°), then click Capture.';
}

function capture() {
  fetch('/calibrate/sample').then(function(r) { return r.json(); }).then(function(j) {
    var deg = Math.round(idx * 360 / count);
    positions.push({ deg: deg, adc: j.adc });
    idx++;
    if (idx < count) {
      showStep();
    } else {
      document.getElementById('step').style.display = 'none';
      document.getElementById('done').style.display = 'block';
      renderTable();
    }
  }).catch(function(e) {
    document.getElementById('msg').textContent = 'Capture failed: ' + e;
  });
}

function renderTable() {
  var html = '<tr><th>Direction</th><th>ADC</th></tr>';
  positions.forEach(function(p) {
    html += '<tr><td>' + compassLabel(p.deg) + ' (' + p.deg + '°)</td><td>' + p.adc + '</td></tr>';
  });
  document.getElementById('resultTable').innerHTML = html;
}

function save() {
  fetch('/calibrate/save', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ positions: positions })
  }).then(function(r) { return r.text(); }).then(function(t) {
    document.getElementById('msg').textContent = t;
  }).catch(function(e) {
    document.getElementById('msg').textContent = 'Save failed: ' + e;
  });
}

// Live ADC preview while positioning the vane, before Capture is clicked.
setInterval(function() {
  if (document.getElementById('step').style.display === 'none') return;
  fetch('/calibrate/sample').then(function(r) { return r.json(); }).then(function(j) {
    document.getElementById('liveAdc').textContent = j.adc;
  }).catch(function() {});
}, 1000);
</script>
</body>
</html>
)CALPAGE";
