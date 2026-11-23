-- Run through reaper-test on Windows, with JS_ReaScriptAPI available.
local r=reaper
local dir=r.GetResourcePath()..'/Projects/seek-click'
r.RecursiveCreateDirectory(dir,0)
local function wav(name,n)
 local old=io.open(dir..'/'..name,'rb');if old then old:close();return end;local f=assert(io.open(dir..'/'..name,'wb'))
 f:write('RIFF',string.pack('<I4',36+n*2),'WAVEfmt ',string.pack('<I4I2I2I4I4I2I2',16,1,1,12000,24000,2,16),'data',string.pack('<I4',n*2),string.rep(string.pack('<i2',4000),n));f:close()
end
wav('01_old.wav',12000);wav('02_short.wav',960);wav('03_long.wav',48000)
local h=r.OpenMediaExplorer(dir..'/01_old.wav',false)
local list=assert(r.JS_Window_FindChildByID(h,1001))
local fader=assert(r.JS_Window_FindChildByID(h,1045))
local function msg(win,m,w,p) return r.JS_WindowMessage_Send(win,m,w or 0,0,p or 0,0) end
local function cmd(n)msg(h,'WM_COMMAND',n)end
local autoplay=r.GetToggleCommandStateEx(32063,1011)
local repeatPreview=r.GetToggleCommandStateEx(32063,1068)
r.atexit(function()
  cmd(1009)
  if autoplay>=0 then cmd(autoplay==1 and 40035 or 40036) end
  if repeatPreview>=0 then cmd(repeatPreview==1 and 40039 or 40040) end
end)
local function status()return r.GetExtState('MediaWaveThumbnails','status')end
local function num(k)return tonumber(status():match('"'..k..'":([%d%.]+)'))end
local function point(name,fraction,sub)
 local row
 for i=0,r.JS_ListView_GetItemCount(list)-1 do if r.JS_ListView_GetItemText(list,i,0):find(name,1,true) then row=i;break end end
 assert(row,name)
 local m=r.JS_Mem_Alloc(16)
 r.JS_Mem_FromString(m,0,string.pack('<i4i4i4i4',0,sub or num('column'),0,0),16)
 assert(msg(list,'0x1038',row,r.JS_Window_AddressFromHandle(m))~=0)
 local a,b=r.JS_String(m,0,16)
 local l,t,rr,bb=string.unpack('<i4i4i4i4',type(a)=='string' and a or b)
 r.JS_Mem_Free(m)
 return math.floor(l+6+fraction*(rr-l-13)) | (math.floor((t+bb)/2)<<16)
end
local function last()local _,path,_,_,_,_,vol=r.MediaExplorerGetLastPlayedFileInfo();return path,vol end
local stage,at,p,done,original=0,r.time_precise()
local function nextStage(n)stage,at=n,r.time_precise()end
local function tick()
 assert(r.time_precise()-at<10,'phase '..stage..' '..status())
 if stage==0 and (num('published_ready')or 0)>=3 then
  r.OpenMediaExplorer(dir..'/01_old.wav',true);cmd(40035);cmd(40040);cmd(1009); print('before',last())
  p=point('02_short',.6);done=num('seek_completed');original=msg(fader,'0x400')
  r.JS_WindowMessage_Post(list,'WM_LBUTTONDOWN',1,0,p&0xffff,p>>16)
  nextStage(1)
 elseif stage==1 and r.time_precise()-at>.2 then
  local path,gain=last()
  if (path or ''):find('02_short',1,true) then
    assert(gain<1e-10,'Early preview was audible at gain '..gain)
  end
  print('Before release',path,gain)
  r.JS_WindowMessage_Post(list,'WM_LBUTTONUP',0,0,p&0xffff,p>>16);nextStage(2)
 elseif stage==2 and num('seek_completed')==done+1 then
  local shortPath,shortGain=last()
  print('Short file after seek',shortPath,shortGain)
  assert(shortGain>0.01,'Final short preview remained muted')
  assert((shortPath or ''):find('02_short',1,true),'Wrong preview source')
  assert(r.GetToggleCommandStateEx(32063,1011)==1,'Autoplay preference changed')
  assert(math.abs(num('seek_ratio')-.6)<.025,'Wrong short-file offset')
  assert(msg(fader,'0x400')==original,'Volume not restored')
  p=point('03_long',.75);r.JS_WindowMessage_Post(list,'WM_LBUTTONDOWN',1,0,p&0xffff,p>>16);r.JS_WindowMessage_Post(list,'WM_LBUTTONUP',0,0,p&0xffff,p>>16);nextStage(3)
 elseif stage==3 and num('seek_completed')==done+2 then
  local label=r.JS_Window_GetTitle(r.JS_Window_FindChildByID(h,1016))
  assert((last()or ''):find('03_long',1,true),label)
  local seconds=tonumber(label:match('^0:([%d%.]+)')); assert(seconds and math.abs(seconds-3)<.15,'Wrong seek: '..label)
  assert(r.GetToggleCommandStateEx(32063,1011)==1,'Autoplay preference changed')
  assert(msg(fader,'0x400')==original,'Volume not restored')
  print('PASS 80 ms file reached the requested seek; different file seeks around 3s; Autoplay and volume restored',label)
  cmd(1009);return
 end
 r.defer(tick)
end
tick()
