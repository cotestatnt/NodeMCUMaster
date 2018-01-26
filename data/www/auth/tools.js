var websock;


function staticWifi() {
  if (document.getElementById('wifiSetup').checked) {
    document.getElementById('ipAddress').disabled = false;
    document.getElementById('gateway').disabled = false;
    document.getElementById('subnetmask').disabled = false;
  } else {
    document.getElementById('ipAddress').disabled = true;
    document.getElementById('gateway').disabled = true;
    document.getElementById('subnetmask').disabled = true;
  }
}



function start() {
  websock = new WebSocket('ws://' + window.location.hostname + '/ws');
  websock.onopen = function(evt) {
    console.log('ESP WebSock Open');
    websock.send('{"command":"getconf"}');
  };
  websock.onclose = function(evt) {
    console.log('ESP WebSock Close');
  };
  websock.onerror = function(evt) {
    console.log(evt);
  };
  websock.onmessage = function(evt) {
    obj = JSON.parse(evt.data);
    if (obj.command == "saveconfig") {
      console.log(evt.data);
      listCONF(obj);
    }
  };
}


function listCONF(obj) {
  console.log(obj);
  document.getElementById('adminPswd').value = obj.adminPswd;
  document.getElementById('pin').value = obj.pin;
  
  document.getElementById('ssid').value = obj.ssid;
  document.getElementById('wifipass').value = obj.pswd;
  document.getElementById('gpioStatus').value = obj.gpioStatus;
  document.getElementById('nodeNumber').value = obj.nodeNumber;

  document.getElementById('startTime').value = obj.startTime;
  document.getElementById('stopTime').value = obj.stopTime;
  document.getElementById('pauseTime').value = obj.pauseTime;

  var wifiSetup = !obj.dhcp;
  if (wifiSetup) {
    document.getElementById('wifiSetup').checked = true;
    document.getElementById('ipAddress').disabled = false;
    document.getElementById('gateway').disabled = false;
    document.getElementById('subnetmask').disabled = false;
    document.getElementById('ipAddress').value = obj.ipAddress
    document.getElementById('gateway').value = obj.gateway
    document.getElementById('subnetmask').value = obj.subnetmask
  } else {
    document.getElementById('ipAddress').disabled = true;
    document.getElementById('gateway').disabled = true;
    document.getElementById('subnetmask').disabled = true;
  }
}



function saveConf() {
  var datatosend = {};
  datatosend.command = "saveconfig";
  
  datatosend.adminPswd = document.getElementById('adminPswd').value;
  datatosend.pin = document.getElementById('pin').value;
  
  datatosend.ssid = document.getElementById('ssid').value;;
  datatosend.pswd = document.getElementById('wifipass').value;
  datatosend.dhcp = !(document.getElementById('wifiSetup').checked);

  datatosend.ipAddress = document.getElementById('ipAddress').value;
  datatosend.gateway = document.getElementById('gateway').value;
  datatosend.subnetmask = document.getElementById('subnetmask').value;

  datatosend.gpioStatus = document.getElementById('gpioStatus').value;
  datatosend.nodeNumber = document.getElementById('nodeNumber').value;

  datatosend.stopTime = document.getElementById('stopTime').value;
  datatosend.startTime = document.getElementById('startTime').value;
  datatosend.pauseTime = document.getElementById('pauseTime').value;

  console.log(JSON.stringify(datatosend));
  websock.send(JSON.stringify(datatosend));
}
