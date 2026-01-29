#pragma once

static const char kIndexHtml[] = R"HTML(
<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8"/>
    <meta name="viewport" content="width=device-width,initial-scale=1"/>
    <title>SilkCast API Reference</title>
    <style>
      :root {
        --bg: #ffffff;
        --card: #f8fafc;
        --muted: #64748b;
        --text: #0f172a;
        --accent: #0f172a;
        --accent-2: #3b82f6;
        --border: #e2e8f0;
      }
      * { box-sizing: border-box; }
      body {
        margin: 0;
        font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, sans-serif;
        background: #ffffff;
        color: var(--text);
      }
      .wrap { max-width: 1080px; margin: 32px auto; padding: 0 20px 40px; }
      header { display: flex; align-items: center; justify-content: space-between; gap: 16px; margin-bottom: 24px; }
      h1 { margin: 0; font-size: 28px; letter-spacing: 0.4px; }
      .pill { padding: 6px 10px; border: 1px solid var(--border); border-radius: 999px; color: var(--muted); font-size: 12px; }
      .grid { display: grid; grid-template-columns: 1.3fr 1fr; gap: 24px; }
      .card { background: var(--card); border: 1px solid var(--border); border-radius: 14px; padding: 20px; }
      .card h2 { margin: 0 0 16px; font-size: 18px; }
      
      /* Tabs */
      .tabs { display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 20px; }
      .tabs button {
        border: 1px solid var(--border);
        background: #fff;
        color: var(--muted);
        padding: 8px 14px;
        border-radius: 8px;
        cursor: pointer;
        font-size: 13px;
        transition: all 0.2s;
      }
      .tabs button:hover { background: #f1f5f9; color: var(--text); }
      .tabs button.active { border-color: var(--accent); color: #fff; background: var(--accent); }

      /* Form Elements */
      .param-group { margin-bottom: 24px; }
      .param-row { display: grid; grid-template-columns: 140px 1fr; gap: 12px; margin-bottom: 12px; align-items: center; }
      .param-label { font-size: 13px; color: var(--muted); display: flex; flex-direction: column; }
      .param-desc { font-size: 10px; opacity: 0.7; margin-top: 2px; }
      
      input, select {
        background: #fff;
        color: var(--text);
        border: 1px solid var(--border);
        border-radius: 8px;
        padding: 8px 12px;
        font-size: 13px;
        width: 100%;
        outline: none;
      }
      input:focus, select:focus { border-color: var(--accent-2); }
      input[type="checkbox"] { width: auto; }

      /* Output Section */
      .output-group { display: flex; flex-direction: column; gap: 12px; }
      .url-bar { display: flex; gap: 8px; }
      .url-bar input { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; color: var(--accent-2); }
      button.action {
        background: var(--card);
        border: 1px solid var(--border);
        color: var(--text);
        padding: 0 16px;
        border-radius: 8px;
        cursor: pointer;
        font-weight: 500;
      }
      button.primary { border-color: var(--accent); background: var(--accent); color: #fff; }
      button.primary:hover { opacity: 0.9; }
      
      .qr-container { 
        margin-top: 24px; 
        display: flex; 
        justify-content: center; 
        background: #fff; 
        padding: 16px; 
        border-radius: 12px;
        width: fit-content;
        margin-left: auto;
        margin-right: auto;
      }
      .description-box {
        background: #f1f5f9;
        border-radius: 8px;
        padding: 12px;
        font-size: 13px;
        color: var(--muted);
        margin-bottom: 20px;
        line-height: 1.5;
      }

      @media (max-width: 900px) { .grid { grid-template-columns: 1fr; } }
    </style>
  </head>
  <body>
    <div class="wrap">
      <header>
        <div>
          <h1>SilkCast API</h1>
          <div style="font-size: 13px; color: var(--muted); margin-top: 4px;">Interactive Link Builder & Documentation</div>
        </div>
        <div class="pill" id="statusPill">Loading Schema...</div>
      </header>

      <div class="grid">
        <!-- Left Column: Controls -->
        <div class="card">
          <div class="tabs" id="routeTabs"></div>
          <div id="routeDescription" class="description-box"></div>
          <div id="paramContainer"></div>
        </div>

        <!-- Right Column: Output -->
        <div class="card">
          <h2>Generated Link</h2>
          <div class="output-group">
             <div class="url-bar">
               <input type="text" id="outputUrl" readonly />
               <button class="action primary" id="copyBtn">Copy</button>
               <button class="action" id="openBtn">Open</button>
             </div>
             
             <!-- Host Selection -->
             <div style="margin-top: 12px;">
                <div style="font-size: 12px; color: var(--muted); margin-bottom: 6px;">Host Address</div>
                <div style="display: flex; gap: 8px;">
                  <input id="hostInput" placeholder="192.168.1.x:8080" />
                  <button class="action" id="detectIpBtn">Detect LAN</button>
                </div>
             </div>

             <div class="qr-container">
               <canvas id="qrCanvas"></canvas>
             </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Minimal QR Code Lib -->
    <script>
      /* QR Code Generator (MIT) */
      window.qrcode=function(){var a=function(a,b){this.mode=1,this.data=a,this.parsed=[],this.data=a;for(var c=0;c<this.data.length;c++)this.parsed.push(this.data.charCodeAt(c));this.parsed.length!=this.data.length&&this.parsed.unshift(191),this.getLength=function(){return this.parsed.length},this.write=function(a){for(var b=0;b<this.parsed.length;b++)a.put(this.parsed[b],8)}};return a.prototype={getLength:function(){return this.parsed.length},write:function(a){for(var b=0;b<this.parsed.length;b++)a.put(this.parsed[b],8)}},function(b,c){var d={};d.getBCHTypeInfo=function(a){for(var b=a<<10;b>=1024;)b^=1335<<b.toString(2).length-11;return(a<<10|b)^21522},d.getBCHDigit=function(a){for(var b=0;0!=a;)b++,a>>>=1;return b},d.getPatternPosition=function(a){return[[],[6,18],[6,22],[6,26],[6,30],[6,34],[6,22,38],[6,24,42],[6,26,46],[6,28,50],[6,30,54],[6,32,58],[6,34,62],[6,26,46,66],[6,26,48,70],[6,26,50,74],[6,30,54,78],[6,30,56,82],[6,30,58,86],[6,34,62,90],[6,28,50,72,94],[6,26,50,74,98],[6,30,54,78,102],[6,28,54,80,106],[6,32,58,84,110],[6,30,58,86,114],[6,34,62,90,118],[6,26,50,74,98,122],[6,30,54,78,102,126],[6,26,52,78,104,130],[6,30,56,82,108,134],[6,34,60,86,112,138],[6,30,58,86,114,142],[6,34,62,90,118,146],[6,30,54,78,102,126,150],[6,24,50,76,102,128,154],[6,28,54,80,106,132,158],[6,32,58,84,110,136,162],[6,26,54,82,110,138,166],[6,30,58,86,114,142,170]][a-1]},d.getMask=function(a,b,c){switch(a){case 0:return(b+c)%2==0;case 1:return b%2==0;case 2:return c%3==0;case 3:return(b+c)%3==0;case 4:return(Math.floor(b/2)+Math.floor(c/3))%2==0;case 5:return(b*c)%2+(b*c)%3==0;case 6:return((b*c)%2+(b*c)%3)%2==0;case 7:return((b+c)%2+(b*c)%3)%2==0;default:return!1}},d.getErrorCorrectPolynomial=function(a){for(var b=new e([1],0),c=0;c<a;c++)b=b.multiply(new e([1,g.gexp(c)],0));return b},d.getLengthInBits=function(a,b){if(1<=b&&b<10)switch(a){case 1:return 10;case 2:return 9;case 4:return 8;case 8:return 8;default:return 0}else if(b<27)switch(a){case 1:return 12;case 2:return 11;case 4:return 16;case 8:return 10;default:return 0}else if(b<41)switch(a){case 1:return 14;case 2:return 13;case 4:return 16;case 8:return 12;default:return 0}else return 0},d.getLostPoint=function(a){for(var b=a.getModuleCount(),c=0,d=0;d<b;d++)for(var e=0;e<b;e++){for(var f=0,g=a.isDark(d,e),h=-1;h<=1;h++)if(!(d+h<0||b<=d+h))for(var i=-1;i<=1;i++)e+i<0||b<=e+i||(0==h&&0==i||g==a.isDark(d+h,e+i)&&f++);f>5&&(c+=3+f-5)}for(var d=0;d<b-1;d++)for(var e=0;e<b-1;e++){var j=0;a.isDark(d,e)&&j++,a.isDark(d+1,e)&&j++,a.isDark(d,e+1)&&j++,a.isDark(d+1,e+1)&&j++,(0==j||4==j)&&(c+=3)}for(var d=0;d<b;d++)for(var e=0;e<b-6;e++)a.isDark(d,e)&&!a.isDark(d,e+1)&&a.isDark(d,e+2)&&a.isDark(d,e+3)&&a.isDark(d,e+4)&&!a.isDark(d,e+5)&&a.isDark(d,e+6)&&(c+=40);for(var e=0;e<b;e++)for(var d=0;d<b-6;d++)a.isDark(d,e)&&!a.isDark(d+1,e)&&a.isDark(d+2,e)&&a.isDark(d+3,e)&&a.isDark(d+4,e)&&!a.isDark(d+5,e)&&a.isDark(d+6,e)&&(c+=40);for(var k=0,e=0;e<b;e++)for(var d=0;d<b;d++)a.isDark(d,e)&&k++;var l=Math.abs(100*k/b/b-50)/5;return c+=10*l},draw=function(a){var b=document.createElement("canvas");b.width=a.getModuleCount()*4,b.height=a.getModuleCount()*4;var c=b.getContext("2d");return c.fillStyle="#fff",c.fillRect(0,0,b.width,b.height),c.fillStyle="#000",null};var e=function(a,b){if(void 0==a.length)throw new Error(a.length+"/"+b);for(var c=0;c<a.length&&0==a[c];)c++;this.num=new Array(a.length-c+b);for(var d=0;d<a.length-c;d++)this.num[d]=a[d+c]};e.prototype={get:function(a){return this.num[a]},getLength:function(){return this.num.length},multiply:function(a){for(var b=new Array(this.getLength()+a.getLength()-1),c=0;c<b.length;c++)b[c]=0;for(var c=0;c<this.getLength();c++)for(var d=0;d<a.getLength();d++)b[c+d]^=g.gexp(g.glog(this.get(c))+g.glog(a.get(d)));return new e(b,0)},mod:function(a){if(this.getLength()-a.getLength()<0)return this;for(var b=g.glog(this.get(0))-g.glog(a.get(0)),c=new Array(this.getLength()),d=0;d<this.getLength();d++)c[d]=this.get(d);for(var d=0;d<a.getLength();d++)c[d]^=g.gexp(g.glog(a.get(d))+b);return(new e(c,0)).mod(a)}};var f=function(a,b,c){this.totalCount=a,this.dataCount=b,this.rsBlock=c};f.getRSBlocks=function(a,b){var c=f.getRsBlockTable(a,b);if(void 0==c)throw new Error("bad rs block @ typeNumber:"+a+"/errorCorrectLevel:"+b);for(var d=c.getLength()/3,e=[],g=0;g<d;g++)for(var h=c.get(3*g),i=c.get(3*g+1),j=c.get(3*g+2),k=0;k<h;k++)e.push(new f(i,j));return e},f.getRsBlockTable=function(a,b){switch(b){case 1:return new e([1,26,19],0);case 0:return new e([1,26,16],0);case 3:return new e([1,26,13],0);case 2:return new e([1,26,9],0)}},f.getRsBlockTable=function(a,b){switch(b){case 1:return new e([1,26,19],0);case 0:return new e([1,26,16],0);case 3:return new e([1,26,13],0);case 2:return new e([1,26,9],0)}};var g={glog:function(a){if(a<1)throw new Error("glog("+a+")");return g.LOG_TABLE[a]},gexp:function(a){for(;a<0;)a+=255;for(;a>=256;)a-=255;return g.EXP_TABLE[a]},EXP_TABLE:new Array(256),LOG_TABLE:new Array(256)},h=0;h<8;h++)g.EXP_TABLE[h]=1<<h;for(var h=8;h<256;h++)g.EXP_TABLE[h]=g.EXP_TABLE[h-4]^g.EXP_TABLE[h-5]^g.EXP_TABLE[h-6]^g.EXP_TABLE[h-8];for(var h=0;h<255;h++)g.LOG_TABLE[g.EXP_TABLE[h]]=h;var i=function(a,b){this.typeNumber=a,this.errorCorrectLevel=b,this.modules=null,this.moduleCount=0,this.dataCache=null,this.dataList=[]};return i.prototype={addData:function(b){var c=new a(b);this.dataList.push(c),this.dataCache=null},isDark:function(a,b){return!(a<0||this.moduleCount<=a||b<0||this.moduleCount<=b)&&this.modules[a][b]},getModuleCount:function(){return this.moduleCount},make:function(){this.makeImpl(!1,this.getBestMaskPattern())},makeImpl:function(a,b){this.moduleCount=4*this.typeNumber+17,this.modules=new Array(this.moduleCount);for(var c=0;c<this.moduleCount;c++){this.modules[c]=new Array(this.moduleCount);for(var e=0;e<this.moduleCount;e++)this.modules[c][e]=null}this.setupPositionProbePattern(0,0),this.setupPositionProbePattern(this.moduleCount-7,0),this.setupPositionProbePattern(0,this.moduleCount-7),this.setupPositionAdjustPattern(),this.setupTimingPattern(),this.setupTypeInfo(a,b),this.typeNumber>=7&&this.setupTypeNumber(a),null==this.dataCache&&(this.dataCache=i.createData(this.typeNumber,this.errorCorrectLevel,this.dataList)),this.mapData(this.dataCache,b)},setupPositionProbePattern:function(a,b){for(var c=-1;c<=7;c++)if(!(a+c<=-1||this.moduleCount<=a+c))for(var d=-1;d<=7;d++)b+d<=-1||this.moduleCount<=b+d||(0<=c&&c<=6&&(0==d||6==d)||0<=d&&d<=6&&(0==c||6==c)||2<=c&&c<=4&&2<=d&&d<=4?this.modules[a+c][b+d]=!0:this.modules[a+c][b+d]=!1)},getBestMaskPattern:function(){for(var a=0,b=0,c=0;c<8;c++){this.makeImpl(!0,c);var e=d.getLostPoint(this);(0==c||a>e)&&(a=e,b=c)}return b},createMovieClip:function(a,b,c){var d=a.createEmptyMovieClip(b,c);this.make();for(var e=1,f=0;f<this.modules.length;f++)for(var g=f*e,h=0;h<this.modules[f].length;h++){var i=h*e;this.modules[f][h]&&(d.beginFill(0,100),d.moveTo(i,g),d.lineTo(i+e,g),d.lineTo(i+e,g+e),d.lineTo(i,g+e),d.endFill())}return d},setupTimingPattern:function(){for(var a=8;a<this.moduleCount-8;a++)null==this.modules[a][6]&&(this.modules[a][6]=a%2==0),null==this.modules[6][a]&&(this.modules[6][a]=a%2==0)},setupPositionAdjustPattern:function(){for(var a=d.getPatternPosition(this.typeNumber),b=0;b<a.length;b++)for(var c=0;c<a.length;c++){var e=a[b],f=a[c];if(null==this.modules[e][f])for(var g=-2;g<=2;g++)for(var h=-2;h<=2;h++)-2==g||2==g||-2==h||2==h||0==g&&0==h?this.modules[e+g][f+h]=!0:this.modules[e+g][f+h]=!1}},setupTypeNumber:function(a){for(var b=d.getBCHTypeInfo(this.typeNumber),c=0;c<18;c++){var e=!a&&(b>>c&1)==1;this.modules[Math.floor(c/3)][c%3+this.moduleCount-8-3]=e,this.modules[c%3+this.moduleCount-8-3][Math.floor(c/3)]=e}},setupTypeInfo:function(a,b){for(var c=this.errorCorrectLevel<<3|b,e=d.getBCHTypeInfo(c),f=0;f<15;f++){var g=!a&&(e>>f&1)==1;f<6?this.modules[f][8]=g:f<8?this.modules[f+1][8]=g:this.modules[this.moduleCount-15+f][8]=g}for(var f=0;f<15;f++){var g=!a&&(e>>f&1)==1;f<8?this.modules[8][this.moduleCount-f-1]=g:f<9?this.modules[8][15-f-1+1]=g:this.modules[8][15-f-1]=g}this.modules[this.moduleCount-8][8]=!a},mapData:function(a,b){for(var c=-1,e=this.moduleCount-1,f=7,g=0,h=this.moduleCount-1;h>0;h-=2)for(6==h&&h--;;){for(var i=0;i<2;i++)if(null==this.modules[e][h-i]){var j=!1;g<a.length&&(j=(a[g]>>>f&1)==1);var k=d.getMask(b,e,h-i);k&&(j=!j),this.modules[e][h-i]=j,f--, -1==f&&(g++,f=7)}e+=c;if(e<0||this.moduleCount<=e){e-=c,c=-c;break}}}},i.createData=function(a,b,c){for(var d=f.getRSBlocks(a,b),g=new e([],0),h=0;h<c.length;h++){var i=c[h];g.put(i.mode,4),g.put(i.getLength(),d.getLengthInBits(i.mode,a)),i.write(g)}for(var j=0,h=0;h<d.length;h++)j+=d[h].dataCount;if(g.length>8*j)throw new Error("code length overflow. ("+g.length+">"+8*j+")");for(g.length+4<=8*j&&g.put(0,4);g.length%8!=0;)g.putBit(!1);for(; ;){if(g.length>=8*j)break;if(g.put(236,8),g.length>=8*j)break;g.put(17,8)}return i.createBytes(g,d)},i.createBytes=function(a,b){for(var c=0,d=0,f=0,h=new Array(b.length),i=new Array(b.length),j=0;j<b.length;j++){var k=b[j].dataCount,l=b[j].totalCount-k;d=Math.max(d,k),f=Math.max(f,l),h[j]=new Array(k);for(var m=0;m<h[j].length;m++)h[j][m]=255&a.buffer[m+c];c+=k;var n=d.getErrorCorrectPolynomial(l),o=new e(h[j],n.getLength()-1),p=o.mod(n);i[j]=new Array(n.getLength()-1);for(var m=0;m<i[j].length;m++){var q=m+p.getLength()-i[j].length;i[j][m]=q>=0?p.get(q):0}}for(var r=0,j=0;j<b.length;j++)r+=b[j].totalCount;for(var s=new Array(r),t=0,m=0;m<d;m++)for(var j=0;j<b.length;j++)m<h[j].length&&(s[t++]=h[j][m]);for(var m=0;m<f;m++)for(var j=0;j<b.length;j++)m<i[j].length&&(s[t++]=i[j][m]);return s},i}();
    </script>
    
    <script>
      let schema = [];
      let currentRoute = null;
      let devices = [];
      let inputValues = {}; // map: routePath -> { paramName -> value }

      // DOM Elements
      const routeTabs = document.getElementById('routeTabs');
      const paramContainer = document.getElementById('paramContainer');
      const statusPill = document.getElementById('statusPill');
      const outputUrl = document.getElementById('outputUrl');
      const hostInput = document.getElementById('hostInput');
      const routeDescription = document.getElementById('routeDescription');
      const copyBtn = document.getElementById('copyBtn');
      const openBtn = document.getElementById('openBtn');
      const qrCanvas = document.getElementById('qrCanvas');

      async function init() {
        // Set Host
        hostInput.value = window.location.host;
        hostInput.addEventListener('input', updateOutput);
        
        try {
          // Fetch Schema
          const res = await fetch('/api/schema');
          if (!res.ok) throw new Error('Failed to fetch schema');
          schema = await res.json();
          statusPill.textContent = 'Active';
          statusPill.style.color = '#10b981';
          statusPill.style.borderColor = '#10b981';

          // Fetch Devices for dropdowns
          await fetchDevices();
          
          renderTabs();
          if (schema.length > 0) selectRoute(schema[0]);
        } catch (e) {
          statusPill.textContent = 'Error';
          console.error(e);
          routeTabs.innerHTML = '<div style="color: coral; padding: 10px;">Failed to load API Schema. Is the server running?</div>';
        }
      }

      async function fetchDevices() {
        try {
          const res = await fetch('/device/list');
          if(res.ok) {
            devices = await res.json();
          } else {
            devices = ['video0'];
          }
        } catch(e) {
          devices = ['video0'];
        }
      }

      function renderTabs() {
        routeTabs.innerHTML = '';
        schema.forEach(route => {
          // Heuristic to make tab names nicer
          const name = route.path.replace('/stream/', '').replace('/device/', '').replace(/\/\{.*?\}/g, '');
          const btn = document.createElement('button');
          btn.textContent = route.method + ' ' + name;
          btn.onclick = () => selectRoute(route);
          routeTabs.appendChild(btn);
        });
      }

      function selectRoute(route) {
        currentRoute = route;
        
        // Update Tabs style
        Array.from(routeTabs.children).forEach(btn => {
           btn.classList.toggle('active', btn.textContent.includes(route.path.replace('/stream/', ''))); // Simple fuzzy match or use index
        });
        
        // Use index to strict match active tab
        const idx = schema.indexOf(route);
        if (routeTabs.children[idx]) {
             Array.from(routeTabs.children).forEach(b => b.classList.remove('active'));
             routeTabs.children[idx].classList.add('active');
        }

        renderParams(route);
        updateOutput();
      }

      function renderParams(route) {
        paramContainer.innerHTML = '';
        routeDescription.textContent = route.description || 'No description provided.';
        
        // Ensure storage for this route params
        if (!inputValues[route.path]) inputValues[route.path] = {};

        // 1. Path Params (e.g. {device})
        const pathPars = (route.path.match(/\{([a-zA-Z0-9_]+)\}/g) || []).map(s => s.slice(1,-1));
        const paramMeta = new Map();
        if (route.params) {
          route.params.forEach(p => paramMeta.set(p.name, p));
        }
        
        pathPars.forEach(name => {
           // Prefer explicit schema metadata when available.
           const meta = paramMeta.get(name);
           const isDevice = name === 'device';
           const row = createParamRow(
             name,
             meta ? meta.type : (isDevice ? 'device' : 'string'),
             meta ? meta.default : '',
             meta ? meta.description : 'URL Parameter',
             meta ? meta.options : undefined
           );
           paramContainer.appendChild(row);
        });

        // 2. Query Params
        if (route.params) {
          route.params.forEach(p => {
             // Skip if this param is already handled in path
             if (pathPars.includes(p.name)) return;
             const row = createParamRow(p.name, p.type, p.default, p.description, p.options);
             paramContainer.appendChild(row);
          });
        }
      }

      function createParamRow(name, type, def, desc, options) {
         const div = document.createElement('div');
         div.className = 'param-row';
         
         const labelDiv = document.createElement('div');
         labelDiv.className = 'param-label';
         labelDiv.innerHTML = `<span>${name}</span><span class="param-desc">${desc || ''}</span>`;
         
         div.appendChild(labelDiv);
         
         let input;
         // Check if we have a saved value, else use default
         const val = (inputValues[currentRoute.path] && inputValues[currentRoute.path][name] !== undefined) 
                     ? inputValues[currentRoute.path][name] 
                     : def;
         const value = (val === undefined || val === null) ? '' : val;

         if (type === 'device') {
           input = document.createElement('select');
           if (devices.length === 0) devices = ['video0'];
           devices.forEach(d => {
             const opt = document.createElement('option');
             opt.value = d;
             opt.textContent = d;
             input.appendChild(opt);
           });
           input.value = value || devices[0];
         } else if (type === 'select') {
           input = document.createElement('select');
           if (options) {
             options.forEach(o => {
               const opt = document.createElement('option');
               opt.value = o;
               opt.textContent = o;
               input.appendChild(opt);
             });
           }
           input.value = value;
         } else if (type === 'bool') {
           input = document.createElement('input');
           input.type = 'checkbox';
           input.checked = val === 'true' || val === true;
           // wrapper to align checkbox
           // actually standard input style matches
         } else if (type === 'int') {
           input = document.createElement('input');
           input.type = 'number';
           input.value = value;
         } else {
           input = document.createElement('input');
           input.type = 'text';
           input.value = value;
         }

         // Status listener
         input.addEventListener('input', (e) => {
            if (!inputValues[currentRoute.path]) inputValues[currentRoute.path] = {};
            const v = input.type === 'checkbox' ? input.checked : input.value;
            inputValues[currentRoute.path][name] = v;
            updateOutput();
         });
         
         // For select change
         if (input.tagName === 'SELECT') {
            input.addEventListener('change', () => {
              if (!inputValues[currentRoute.path]) inputValues[currentRoute.path] = {};
              inputValues[currentRoute.path][name] = input.value;
              updateOutput();
            });
         }

         div.appendChild(input);
         return div;
      }

      function updateOutput() {
        if (!currentRoute) return;
        
        let path = currentRoute.path;
        let query = [];
        const host = hostInput.value.trim() || window.location.host;
        const protocol = window.location.protocol;
        
        // Values for this route
        const vals = inputValues[currentRoute.path] || {};
        const paramMeta = new Map();
        if (currentRoute.params) {
          currentRoute.params.forEach(p => paramMeta.set(p.name, p));
        }
        
        // Replace path params
        // Extract needed path keys
        const pathPars = (path.match(/\{([a-zA-Z0-9_]+)\}/g) || []).map(s => s.slice(1,-1));
        pathPars.forEach(key => {
           let v = vals[key];
           // Defaults for path params if missing
           if (v === undefined || v === '') {
             if (key === 'device' && devices.length > 0) {
               v = devices[0];
             } else {
               const meta = paramMeta.get(key);
               if (meta && meta.default !== undefined && meta.default !== '') {
                 v = meta.default;
               } else {
                 v = 'default';
               }
             }
           }
           path = path.replace(`{${key}}`, encodeURIComponent(v));
        });

        // Add Query Params
        if (currentRoute.params) {
          currentRoute.params.forEach(p => {
             // Only add if different from default? Or always?
             // Users usually want to see explicit params. 
             // Logic: if it's in `vals`, use it. if not, use `p.default`.
             // If input is empty string and default is empty, maybe skip?
             // Let's explicitly include everything for clarity in Reference.
             let v = vals[p.name];
             if (v === undefined) v = p.default;
             
             // Checkbox boolean
             if (p.type === 'bool' && (v === false || v === 'false')) return; // usually omit false flags
             if (p.type === 'bool') v = 'true';

             if (v !== '' && v !== null && v !== undefined) {
               query.push(`${encodeURIComponent(p.name)}=${encodeURIComponent(v)}`);
             }
          });
        }
        
        const fullUrl = `${protocol}//${host}${path}` + (query.length ? '?' + query.join('&') : '');
        outputUrl.value = fullUrl;
        
        renderQR(fullUrl);
      }

      function renderQR(text) {
         try {
           const qrFactory = window.qrcode;
           if (typeof qrFactory !== 'function') {
             console.warn('QR library unavailable');
             const ctx = qrCanvas.getContext('2d');
             ctx.clearRect(0, 0, qrCanvas.width, qrCanvas.height);
             return;
           }
           const qr = qrFactory(0, 'M');
           qr.addData(text);
           qr.make();
           const size = qr.getModuleCount();
           const scale = 4;
           qrCanvas.width = size * scale;
           qrCanvas.height = size * scale;
           const ctx = qrCanvas.getContext('2d');
           ctx.fillStyle = '#fff';
           ctx.fillRect(0,0, qrCanvas.width, qrCanvas.height);
           ctx.fillStyle = '#000';
           for (let r=0; r<size; r++) {
             for (let c=0; c<size; c++) {
               if (qr.isDark(r,c)) {
                 ctx.fillRect(c*scale, r*scale, scale, scale);
               }
             }
           }
         } catch(e) {
           console.log("QR Gen Error", e);
         }
      }

      copyBtn.onclick = () => {
         outputUrl.select();
         document.execCommand('copy');
         copyBtn.textContent = 'Copied!';
         setTimeout(() => copyBtn.textContent = 'Copy', 1500);
      };

      openBtn.onclick = () => {
        window.open(outputUrl.value, '_blank');
      };

      document.getElementById('detectIpBtn').onclick = () => {
         const RTCPeer = window.RTCPeerConnection || window.webkitRTCPeerConnection;
         if (!RTCPeer) return;
         const pc = new RTCPeer({iceServers:[]});
         pc.createDataChannel('');
         pc.createOffer().then(o => pc.setLocalDescription(o));
         pc.onicecandidate = (e) => {
            if (e.candidate) {
               const parts = e.candidate.candidate.split(' ');
               const ip = parts[4];
               if (ip && ip.indexOf(':') === -1 && ip !== '127.0.0.1' && (ip.startsWith('192') || ip.startsWith('10'))) {
                  const port = window.location.port ? ':' + window.location.port : '';
                  hostInput.value = ip + port;
                  updateOutput();
                  pc.close();
               }
            }
         };
      };

      init();
    </script>
  </body>
</html>
)HTML";
