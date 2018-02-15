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
  
  document.getElementById('nodeNumber').value = obj.nodeNumber;
  document.getElementById('zone1').value = obj.zone1;
  document.getElementById('zone2').value = obj.zone2;
  document.getElementById('startZ1').value = obj.startZ1;
  document.getElementById('stopZ1').value = obj.stopZ1;
  document.getElementById('startZ2').value = obj.startZ2;
  document.getElementById('stopZ2').value = obj.stopZ2;
  
  document.getElementById('n1').checked = obj.n1;
  document.getElementById('p1').checked = obj.p1;
  document.getElementById('a1').checked = obj.a1;
  document.getElementById('n2').checked = obj.n2;
  document.getElementById('p2').checked = obj.p2;
  document.getElementById('a2').checked = obj.a2;

  document.getElementById('pauseTime').value = obj.pauseTime;
  document.getElementById('hornTime').value = obj.hornTime;
  
  document.getElementById('phone1').value = obj.phone1;
  document.getElementById('phone2').value = obj.phone2;
  document.getElementById('pushDevId').value = obj.pushDevId;
  
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

  datatosend.nodeNumber = document.getElementById('nodeNumber').value;
  datatosend.zone1 = document.getElementById('zone1').value;  
  datatosend.zone2 = document.getElementById('zone2').value;  
  datatosend.stopZ1 = document.getElementById('stopZ1').value;
  datatosend.startZ1 = document.getElementById('startZ1').value;
  datatosend.stopZ2 = document.getElementById('stopZ2').value;
  datatosend.startZ2 = document.getElementById('startZ2').value;
  
  datatosend.n1 = document.getElementById('n1').checked;
  datatosend.p1 = document.getElementById('p1').checked;
  datatosend.a1 = document.getElementById('a1').checked;
  datatosend.n2 = document.getElementById('n2').checked;
  datatosend.p2 = document.getElementById('p2').checked;
  datatosend.a2 = document.getElementById('a2').checked;
   
  datatosend.pauseTime = document.getElementById('pauseTime').value;
  datatosend.hornTime = document.getElementById('hornTime').value;
  
  datatosend.phone1 = document.getElementById('phone1').value;
  datatosend.phone2 = document.getElementById('phone2').value;
  datatosend.pushDevId = document.getElementById('pushDevId').value;
  
  console.log(JSON.stringify(datatosend));
  websock.send(JSON.stringify(datatosend));
}
