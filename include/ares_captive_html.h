#ifndef ARES_CAPTIVE_HTML_H
#define ARES_CAPTIVE_HTML_H

#include <Arduino.h>

// [PORTAL] Página pensada para quem está numa emergência:
// poucos passos, letras grandes, linguagem simples e confirmação clara de que o pedido chegou.
const char ARES_CAPTIVE_INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Pedir socorro</title>
<style>
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,Roboto,Arial,sans-serif;background:#fff;color:#111;font-size:19px;line-height:1.4}
main{max-width:560px;margin:0 auto;padding:16px}
h1{font-size:30px;margin:4px 0 2px;color:#b00020}
.sub{margin:0 0 14px;color:#333;font-size:17px}
.alerta{display:none;background:#fff3cd;border:3px solid #e0a800;border-radius:12px;padding:12px;margin-bottom:14px}
.alerta b{display:block;font-size:20px;margin-bottom:4px}
h2{font-size:21px;margin:20px 0 8px}
.n{display:inline-block;width:30px;height:30px;border-radius:50%;background:#111;color:#fff;text-align:center;line-height:30px;font-size:17px;margin-right:6px}
.opc{display:block;width:100%;text-align:left;font-size:20px;padding:15px 14px;margin:0 0 9px;border:3px solid #ccc;border-radius:12px;background:#f7f7f7;color:#111}
.opc.sel{border-color:#b00020;background:#ffe5e9;font-weight:700}
.opc.r{border-color:#e6a1ab}
input,textarea{width:100%;font-size:20px;padding:13px;border:3px solid #999;border-radius:12px;font-family:inherit}
input:focus,textarea:focus{outline:none;border-color:#111}
.dica{font-size:16px;color:#444;margin:5px 0 0}
.chips{display:flex;gap:8px;flex-wrap:wrap}
.chip{flex:1;min-width:64px;font-size:20px;padding:13px 6px;border:3px solid #ccc;border-radius:12px;background:#f7f7f7;color:#111}
.chip.sel{border-color:#111;background:#111;color:#fff;font-weight:700}
#enviar{display:block;width:100%;margin:22px 0 6px;font-size:25px;font-weight:800;padding:20px;border:0;border-radius:14px;background:#b00020;color:#fff}
#enviar:disabled{background:#bbb;color:#555}
#falta{text-align:center;font-size:16px;color:#444;margin:0}
.cont{text-align:right;font-size:15px;color:#555;margin-top:4px}
.cont.max{color:#b00020;font-weight:700}
#estado{display:none;border-radius:14px;padding:16px;margin:18px 0;border:3px solid #999;font-size:20px}
#estado b{display:block;font-size:23px;margin-bottom:4px}
#estado.e1{background:#eef4ff;border-color:#3b6fd8}
#estado.e2{background:#eef4ff;border-color:#3b6fd8}
#estado.e3{background:#e6f6ea;border-color:#1e8e3e}
#estado.e0{background:#ffe5e9;border-color:#b00020}
.hist{font-size:16px;color:#333;padding:0;margin:0;list-style:none}
.hist li{border-top:1px solid #ddd;padding:8px 0}
.seg{background:#f2f2f2;border-radius:12px;padding:12px 14px;font-size:17px;margin-top:22px}
.seg ul{margin:6px 0 0;padding-left:20px}
footer{font-size:14px;color:#666;text-align:center;margin:20px 0 8px}
</style>
</head>
<body>
<main>
  <h1>Pedir socorro</h1>
  <p class="sub">Funciona <b>sem internet</b> e sem sinal de celular. Seu pedido passa de aparelho em aparelho até a equipe de resgate.</p>

  <div class="alerta" id="alerta" role="alert"><b>📢 Mensagem da equipe de resgate</b><span id="alertaTxt"></span></div>

  <div id="estado" role="status" aria-live="assertive"></div>

  <h2><span class="n">1</span>O que está acontecendo?</h2>
  <button type="button" class="opc r" data-p="0" data-c="RISCO DE VIDA">🆘 Risco de vida agora<br><small>preso, afogando, sem respirar, incêndio</small></button>
  <button type="button" class="opc" data-p="1" data-c="FERIDO">🩹 Tem alguém ferido</button>
  <button type="button" class="opc" data-p="1" data-c="PRECISA AJUDA P/ SAIR">♿ Idoso, criança ou pessoa que não consegue sair</button>
  <button type="button" class="opc" data-p="2" data-c="SEM AGUA/COMIDA">💧 Sem água, comida ou remédio</button>
  <button type="button" class="opc" data-p="3" data-c="OUTRO">💬 Outra coisa / estou bem, mas preciso de apoio</button>

  <h2><span class="n">2</span>Onde você está?</h2>
  <input id="local" maxlength="60" autocomplete="off" placeholder="Ex.: Rua das Flores 120, telhado da casa azul">
  <p class="dica">Rua, número, ponto de referência, andar. Quanto mais fácil de achar, melhor.</p>

  <h2><span class="n">3</span>Quantas pessoas?</h2>
  <div class="chips" id="pessoas">
    <button type="button" class="chip" data-v="1">1</button>
    <button type="button" class="chip" data-v="2">2</button>
    <button type="button" class="chip" data-v="3">3</button>
    <button type="button" class="chip" data-v="4">4</button>
    <button type="button" class="chip" data-v="5+">5+</button>
  </div>

  <h2>Mais alguma coisa? <small style="font-weight:400">(se quiser)</small></h2>
  <textarea id="det" rows="2" maxlength="80" placeholder="Ex.: água subindo, 1 bebê, pessoa com diabetes"></textarea>
  <div class="cont" id="cont"></div>

  <button type="button" id="enviar" disabled>ENVIAR PEDIDO DE SOCORRO</button>
  <p id="falta">Escolha o que está acontecendo e diga onde você está.</p>

  <div id="histBox" style="display:none">
    <h2>Pedidos enviados deste celular</h2>
    <ul class="hist" id="hist"></ul>
  </div>

  <div class="seg">
    <b>Enquanto espera:</b>
    <ul>
      <li>Fique num lugar alto e seguro, se puder.</li>
      <li>Deixe esta página aberta para ver a resposta.</li>
      <li>Economize bateria: baixe o brilho da tela.</li>
      <li>Se conseguir ligar: <b>193</b> Bombeiros, <b>192</b> SAMU, <b>199</b> Defesa Civil.</li>
    </ul>
  </div>
  <footer>Rede ARES • <span id="no">conectando…</span></footer>
</main>
<script>
var LIM=95; // bytes que cabem no rádio (text_message[96])
var sel=null, pessoas='', enviando=false, pend=null, hist=[];
function $(i){return document.getElementById(i)}
try{hist=JSON.parse(localStorage.getItem('ares_hist')||'[]')}catch(e){hist=[]}
function salvar(){try{localStorage.setItem('ares_hist',JSON.stringify(hist.slice(0,10)))}catch(e){}}
function bytes(s){try{return new TextEncoder().encode(s).length}catch(e){return unescape(encodeURIComponent(s)).length}}
function cortar(s,n){while(s.length&&bytes(s)>n)s=s.slice(0,-1);return s}
function montar(){
  var p=[sel?sel.c:'', pessoas?pessoas+' pessoa(s)':'', $('local').value.trim()];
  var d=$('det').value.trim();
  var t=p.filter(function(x){return x}).join(' | ');
  if(d) t+=' | '+d;
  return t;
}
function atualizar(){
  var t=montar(), b=bytes(t), ok=sel&&$('local').value.trim().length>=3;
  var c=$('cont'); c.textContent=b>LIM?'Texto longo: o final será cortado. Encurte se puder.':'';
  c.className='cont'+(b>LIM?' max':'');
  $('enviar').disabled=!ok||enviando;
  $('falta').textContent=ok?'':(!sel?'Escolha o que está acontecendo.':'Diga onde você está.');
}
document.querySelectorAll('.opc').forEach(function(b){b.onclick=function(){
  document.querySelectorAll('.opc').forEach(function(x){x.classList.remove('sel')});
  b.classList.add('sel'); sel={p:+b.dataset.p,c:b.dataset.c}; atualizar();
  $('local').focus();
}});
document.querySelectorAll('.chip').forEach(function(b){b.onclick=function(){
  var j=b.classList.contains('sel');
  document.querySelectorAll('.chip').forEach(function(x){x.classList.remove('sel')});
  if(!j){b.classList.add('sel');pessoas=b.dataset.v}else pessoas=''; atualizar();
}});
$('local').oninput=atualizar; $('det').oninput=atualizar;
var TXT={
 1:['📡 Pedido enviado','Procurando a equipe de resgate pela rede… Deixe esta página aberta.'],
 2:['📦 Pedido guardado na rede','Outro aparelho da rede guardou seu pedido e vai levar até a equipe.'],
 3:['✅ A equipe de resgate recebeu seu pedido','Fique onde está, se for seguro. Deixe esta página aberta para ver respostas.']
};
function mostrar(st,extra){
  var e=$('estado'); e.style.display='block'; e.className='e'+st; e.textContent='';
  var b=document.createElement('b'), s=document.createElement('span');
  if(st===0){b.textContent='❌ Não foi possível enviar';s.textContent=extra||'Confira se ainda está conectado à rede ARES e toque em ENVIAR de novo.'}
  else{b.textContent=TXT[st][0];s.textContent=TXT[st][1]}
  e.appendChild(b);e.appendChild(s); e.scrollIntoView({behavior:'smooth',block:'start'});
}
function hora(t){var d=new Date(t);return ('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)}
function desenharHist(){
  var ul=$('hist'); ul.textContent='';
  $('histBox').style.display=hist.length?'block':'none';
  hist.forEach(function(h){var li=document.createElement('li');
    li.textContent=hora(h.t)+' — '+(h.st===3?'✅ recebido pela equipe':h.st===2?'📦 guardado na rede':'📡 enviado')+' — '+h.txt;
    ul.appendChild(li)});
}
$('enviar').onclick=function(){
  if(enviando)return;
  var t=cortar(montar(),LIM); if(!t||!sel)return;
  enviando=true; $('enviar').textContent='ENVIANDO…'; atualizar();
  var x=new XMLHttpRequest(); x.open('POST','/api/send'); x.timeout=10000;
  x.setRequestHeader('Content-Type','application/json');
  x.onload=function(){
    var r=null; try{r=JSON.parse(x.responseText)}catch(e){}
    if(x.status===200&&r&&r.msg_id){
      pend=r.msg_id; hist.unshift({id:r.msg_id,txt:t,t:Date.now(),st:1}); salvar(); desenharHist(); mostrar(1);
      $('det').value='';
    }else mostrar(0);
    fim();
  };
  x.onerror=x.ontimeout=function(){mostrar(0);fim()};
  x.send(JSON.stringify({priority:sel.p,text:t}));
};
function fim(){enviando=false;$('enviar').textContent='ENVIAR PEDIDO DE SOCORRO';atualizar()}
function status(){
  var x=new XMLHttpRequest(); x.open('GET','/api/status'); x.timeout=5000;
  x.onload=function(){try{
    var d=JSON.parse(x.responseText);
    $('no').textContent='ponto '+(d.nodeId||'?')+(d.battery>=0?' • bateria '+d.battery+'%':'');
    if(d.downstreamMsg){$('alerta').style.display='block';$('alertaTxt').textContent=d.downstreamMsg}
    var m=d.msgs||{}, mud=false;
    hist.forEach(function(h){var s=m[h.id]; if(s&&s>h.st){h.st=s;mud=true; if(h.id===pend)mostrar(s)}});
    if(mud){salvar();desenharHist()}
  }catch(e){}};
  x.send();
}
desenharHist(); atualizar(); status(); setInterval(status,3000);
</script>
</body>
</html>
)rawliteral";

#endif // ARES_CAPTIVE_HTML_H
