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
  document.getElementById('startTimeZ1').value = obj.startTimeZ1;
  document.getElementById('stopTimeZ1').value = obj.stopTimeZ1;
  document.getElementById('startTimeZ2').value = obj.startTimeZ2;
  document.getElementById('stopTimeZ2').value = obj.stopTimeZ2;
  
  document.getElementById('warning1').checked = obj.warning1;
  document.getElementById('alarm1').checked = obj.alarm1;
  document.getElementById('warning2').checked = obj.warning2;
  document.getElementById('alarm2').checked = obj.alarm2;
  
  document.getElementById('pauseTime').value = obj.pauseTime;
  document.getElementById('hornTime').value = obj.hornTime;
  
  document.getElementById('phoneNumber1').value = obj.phoneNumber1;
  document.getElementById('phoneNumber2').value = obj.phoneNumber2;
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
  datatosend.stopTimeZ1 = document.getElementById('stopTimeZ1').value;
  datatosend.startTimeZ1 = document.getElementById('startTimeZ1').value;
  datatosend.stopTimeZ2 = document.getElementById('stopTimeZ2').value;
  datatosend.startTimeZ2 = document.getElementById('startTimeZ2').value;
  
  datatosend.warning1 = document.getElementById('warning1').checked;
  datatosend.alarm1 = document.getElementById('alarm1').checked;
  datatosend.warning2 = document.getElementById('warning2').checked;
  datatosend.alarm2 = document.getElementById('alarm2').checked;
   
  datatosend.pauseTime = document.getElementById('pauseTime').value;
  datatosend.hornTime = document.getElementById('hornTime').value;
  
  datatosend.phoneNumber1 = document.getElementById('phoneNumber1').value;
  datatosend.phoneNumber2 = document.getElementById('phoneNumber2').value;
  datatosend.pushDevId = document.getElementById('pushDevId').value;
  
  console.log(JSON.stringify(datatosend));
  websock.send(JSON.stringify(datatosend));
}
