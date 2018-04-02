#include <WString.h>
#include <RTClib.h>
#include <SD.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <IFTTTMaker.h>
#include <user_interface.h>

#define KEY "dQ3aWB4ewlWxExGizCon_zY9AYxUgDLXJf8NYbMw1I4"  // Get it from this page https://ifttt.com
#define EVENT_NAME "RADALARM" // Set when creating the applet
#define AUDIOPIN  D3
#define PWMPIN  D0
#define INTPIN  D4
#define ALARM_OK       1
#define ALARM_FAIL     2
#define ALARM_FAILMEM  3
#define MAXFNLENGTH 64
#define MAXREBOOTCOUNT 4
#define TUBECONVERSIONFACTOR  0.0057
#define CONTENTBUFFSIZE 4000
#define NVR_ALARM       0  // 1 minute average alarm threshold - 2 bytes
#define NVR_LOCKOUT     2  // Time before another alarm allowed - 2 bytes
#define NVR_AUDIO       4  // Audio enable/disable flag - 1 byte
#define NVR_DELAY       5  // Time to wait before trying a reboot - 1 byte
#define NVR_TIMEOUT     6  // Reboot flag - sets wireless behvior on boot - 1 byte
#define NVR_ALARM_HR_L  7  // 1Hr average low alarm threshold - 2 bytes
#define NVR_ALARM_HR_H  9  // 1Hr average high alarm threshold - 2 bytes
#define NVR_AUDIOALARM  11 // Enable audio alarm - 1 byte
#define WIFITIMEOUTFLAG B10101010

ESP8266WebServer server(80);
RTC_DS1307 rtc;
DateTime now;
volatile unsigned long OneSecTick=0;
unsigned long LastGoodWifi=0;
volatile int MinSecCount=0;
volatile unsigned long Cpm=0;
volatile unsigned long LastCpm=0;
volatile int Cps=0;
volatile long TotCount=0;
volatile float CpmAvg=0;
volatile int HourCount=0;
volatile int Watchdog=180; // 3 minutes - will be 1min after wifi connect
volatile unsigned long HourTotal=0;
float HourAvg=0;
int LastMinute=0;
int LastDay=0;
int BootStatus=0;
unsigned int LoginTimeout=0;
int Units=1;
int PwmWidth;
int AlarmThreshold;
int AlarmThresholdHL;
int AlarmThresholdHH;
int AlarmStatus=0;
int AlarmTryCount=0;
int AudioEnabled=0;
int AudioAlarm=0;
int AudioAlarmCount=0;
volatile int AlarmLockout=0;
volatile bool WifiStarting=false;
int AlarmLockoutTime;
bool OneSecFlag=false;
static const char NavBar[] PROGMEM = "<!DOCTYPE HTML><html lang='en'><head><title>Radiation Monitor</title><meta charset='utf-8'><link rel='stylesheet' href='https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/css/bootstrap.min.css'><script src='https://ajax.googleapis.com/ajax/libs/jquery/3.3.1/jquery.min.js'></script><script src='https://maxcdn.bootstrapcdn.com/bootstrap/3.3.7/js/bootstrap.min.js'></script></head><BODY onload='process()'><nav class='navbar navbar-default'><div class='container-fluid'><div class='navbar-header'><a class='navbar-brand' href='/'>Radiation Monitor</a></div><ul class='nav navbar-nav'><li><a href='/today'>Today</a></li><li><a href='/rangeLastMonth'>Month</a></li><li><a href='/archive.htm'>Archive</a></li><li><a href='/admin.htm'>Admin</a></li><li><a href='/help.htm'>Help</a></li></ul></div></nav>";
static const char CcsStyle[] PROGMEM = "<style>td { padding-right: 30px; } table {margin-left:40px;font-size:20px; } p {margin-left:40px;font-size:15px; } p2 {margin-left:40px;font-size:25px; } a {color: black;} .list-group-item { padding: 5px 20px}</style>";
static String Months[]={"January","February","March","April","May","June","July","August","September","October","November","December"};
static int MonthDays[]={31,28,31,30,31,30,31,31,30,31,30,31};

String Ssid="";
String WifiPw="";
String AdminPw="";
String AlarmMessage="";
const char CompileDate[] = __DATE__ " " __TIME__;

String MonthStr(int Month)
{
  if (Month<1 || Month>12) return "???";
  return Months[Month-1];
}

void ServiceAlarm()  // IFTTT notification
{
  if (AlarmMessage!="")
  {
    WiFiClientSecure client;
    IFTTTMaker ifttt(KEY, client); 
    LogString(String(F("Alarm: "))+AlarmMessage+"\n",1);
    Serial.printf("Mem:%d\n",FreeMem());
    if (AudioAlarm) AudioAlarmCount=8;
    if (FreeMem()>26000)
    {
        if (ifttt.triggerEvent(EVENT_NAME,AlarmMessage))
        {
          LogString(F("Alarm Sent\n"),1);
          AlarmStatus=ALARM_OK;
          AlarmMessage="";
          return;
        }
        else
        {
          LogString(F("Alarm Send fail (API)\n"),1);
          AlarmStatus=ALARM_FAIL;
          if (AlarmTryCount)  AlarmTryCount--;
          if (!AlarmTryCount) AlarmMessage="";
          return;
        }
    }
    AlarmStatus=ALARM_FAILMEM;
    LogString(F("Alarm Send fail (Low Mem)\n"),1);
  }
}

void TriggerAlarm(String Msg,int TryCount,int Lockout)
{
  AlarmMessage=Msg;
  AlarmTryCount=TryCount;
  AlarmLockout=Lockout;
}

void LogString(String Str,int IncTime)
{
  File LogFile = SD.open(F("log.txt"), FILE_WRITE);
  if (LogFile)
  {
    if (IncTime) Str=GetDateTimeStr(0)+" "+String(FreeMem())+" "+Str;
    LogFile.print(Str.c_str());
    LogFile.close();  
    Serial.print(Str.c_str());
  }
}

String GetDateTimeStr(int Res)  // Return current time in various useful formats
{
  char TimeDate[20];
  now = rtc.now();
  if (Res==0) sprintf_P(TimeDate,PSTR("%02d-%02d-%04d %02d:%02d:%02d"), now.day(),now.month(),now.year(),now.hour(),now.minute(),now.second()); // Logging string 
  if (Res==1) sprintf_P(TimeDate,PSTR("%04d-%02d-%02d %02d:%02d"),now.year(),now.month(),now.day(),now.hour(),now.minute());
  if (Res==3) sprintf_P(TimeDate,PSTR("%04d%02d%02d"), now.year(),now.month(),now.day());
  if (Res==4) sprintf_P(TimeDate,PSTR("%04d%02d"), now.year(),now.month());
  return String(TimeDate);  
}

void inline PulseISR(void)
{
  Cps++;
  TotCount++;
  if (AudioEnabled)
  {
      digitalWrite(AUDIOPIN, 0);
      delayMicroseconds(100); // Not ideal, but dont have a spare timer to trigger an int to close
      digitalWrite(AUDIOPIN, 1);
  }
}

void inline TimerISR (void) // Every 1 sec
{
  OneSecFlag=true;
  OneSecTick++; // Global second count
  MinSecCount++; // Current minute
  if (CpmAvg<100 && OneSecTick>30)
    CpmAvg=(CpmAvg*(49)+Cps*60)/50;
  else
    CpmAvg=(CpmAvg*9+Cps*60)/10; 
  Cpm+=Cps;
  HourCount++; // Number of measurements taken so far
  HourTotal+=Cps;
  Cps=0;
  if (MinSecCount>=60)
  {
    LastCpm=Cpm;
    Cpm=0;
    MinSecCount=0;
  }
  if (LoginTimeout>1) LoginTimeout--;
  if (AlarmLockout) AlarmLockout--;
  Watchdog--;
  if (Watchdog<=0) // Timeout! Must reboot.
  {
    if (WifiStarting) // Timed out during wifi setup, must have lost the station
    {
      WriteNVR(NVR_TIMEOUT,WIFITIMEOUTFLAG); // On reboot, dont try to connect to wifi right away
      byte Delay=ReadNVR(NVR_DELAY);
      if (Delay<8) // Calc the delay before trying wifi connect again
      {
        Delay++;
        WriteNVR(NVR_DELAY,Delay);
      }
    }
    Reboot(false);
  }
  timer0_write(ESP.getCycleCount() + 80000000L); // 1 sec
}

void InitSystem() // Read config setup from file
{
  File myFile = SD.open(F("config.txt"), FILE_READ);
  if (myFile) 
  {
    LogString(F("Reading config.txt.."),1);
    while(myFile.available())
    {
      String Line = myFile.readStringUntil('\n');
      int Split=Line.indexOf("=");
      String Key=Line.substring(0,Split);
      String Value=Line.substring(Split+1);
      Key.trim();
      Value.trim();
      if (Key==F("SSID")) Ssid=Value;
      if (Key==F("WIFIPW")) WifiPw=Value;
      if (Key==F("PW")) AdminPw=Value;
    }
    LogString(F("OK\n"),0);
    myFile.close();  
  }
  else
    LogString(F("config.txt not found\n"),1);
}

void InitSDCard()
{
  if (!SD.begin(D8))   Serial.println(F("SD Card Init Fail!"));
}

void InitRTC()
{
  LogString(F("Init RTC.."),0);
  if (rtc.begin())
    LogString(F("OK\n"),0);
  else
    LogString(F("Fail\n"),0);
}

unsigned int FreeMem()
{
  return system_get_free_heap_size();  
}

void MultiBeep(int Number)
{
  for (int i=0;i<Number;i++)
  {
      Beep(100);
      delay(100);
  }
}
void Beep(int Len)
{
  int i;
  for (i=0;i<Len;i++)
  {
      digitalWrite(AUDIOPIN, 0);
      delayMicroseconds(500);
      digitalWrite(AUDIOPIN, 1);
      delayMicroseconds(500);
  }
}


void InitTimers()
{
  noInterrupts();
  timer0_isr_init();
  timer0_attachInterrupt(TimerISR);
  timer0_write(ESP.getCycleCount() + 80000000L); // 1 sec 
  interrupts();
}

// On boot, if wifi doesnt connect, it will stay in wifi setip & WD will then set timeout NVR flag timeout & reboot.
// on reboot, if timeout flag set, dont try to connect to wifi until delay expired, then reboot & try again without timeout flag. Increase delay for next time.
// On boot, if wifi connects ok, reset delay to 0

void InitWifi()
{
  WiFiManager wifiManager;
  if (ReadNVR(NVR_TIMEOUT)!=WIFITIMEOUTFLAG)
  {
    WifiStarting=true;
    wifiManager.autoConnect("AutoConnectAP");
    WifiStarting=false;
    WiFi.mode(WIFI_STA);
    WriteNVR(NVR_DELAY,0); // All ok, set delay to 0 in future
    Serial.printf("Connected to:%s\n",WiFi.SSID().c_str());
    MultiBeep(2); // connected  
  }
  else
  {
     LogString(F("Last wifi start failed, skipping for now...\n"),1);
     MultiBeep(3); // not connected .. for now
  }
  WriteNVR(NVR_TIMEOUT,0);

/*  int Timeout=120;
  WiFi.begin(Ssid.c_str(), WifiPw.c_str());
  WiFi.mode(WIFI_STA);
  LogString(F("Connecting to wifi:"),1);
  while (WiFi.status() != WL_CONNECTED && Timeout)
  {
    LogString(".",0);
    delay(500);
    Timeout--;
  }
  if (WiFi.status() != WL_CONNECTED)
  {
    LogString(F("Failed\n"),0);
    BootStatus=1; // Flag the fail
  }
  else
  {
    LogString(String(F("Success. IP:"))+WiFi.localIP().toString()+"\n",0);
  }*/
}

void SwapUnits()
{
  Units=1-Units;
  LogString("Swap Units ("+server.client().remoteIP().toString()+")",1);  
}

void  InitWebServer()
{
  const char * headerkeys[] = {"Cookie"} ;
  size_t headerkeyssize = sizeof(headerkeys)/sizeof(char*);
  server.onNotFound(HandleNotFound);
  server.collectHeaders(headerkeys, headerkeyssize );
  server.begin();
  LogString(F("Server started\n"),1);
}

void SendRTData()
{
  String XML;
  if (Units)
     XML=String(F("<?xml version='1.0'?> <response> "))+String((CpmAvg)*TUBECONVERSIONFACTOR)+String(F(" uS/Hr</response>"));
   else
     XML=String(F("<?xml version='1.0'?> <response> "))+String(int(CpmAvg))+String(F(" cpm</response> "));
  server.send(200,F("text/xml"),XML);
}


String ConvertDateFormat(String DateStr) // Convert a string from "2018-03-23" to "20180323"
{
  return DateStr.substring(0,4)+DateStr.substring(5,7)+DateStr.substring(8,10);
}


int DaysInMonth(int Month)
{
  return MonthDays[(Month-1)%12];   
}

void ShowGraphPage(String FileName) // Allowed filenames:  "20180323.dat" "/range"(with 3 args) "/rangelastmonth" "/today"
{
  String TempLine="";
  String Content="";
  unsigned long StartTime=millis();
  int TempYear;
  int TempDays;
  int TempMonth;
  File myFile= SD.open(F("template.htm"), FILE_READ);
  if (myFile)
  {
    if (FileName==F("/range")) FileName="/range"+ConvertDateFormat(server.arg(0))+"to"+ConvertDateFormat(server.arg(1))+server.arg(2);
    if (FileName==F("/today")) FileName="/data/"+String(now.year())+"/"+GetDateTimeStr(3)+".dat";
    if (FileName==F("/rangelastmonth")|| FileName==F("/rangelastweek"))
    {
      TempYear=now.year();
      TempMonth=now.month();
      TempDays=now.day();
      if (FileName==F("/rangelastmonth"))
      {
        TempMonth--;
        if (TempMonth<1) { TempMonth=12; TempYear--; }
        if (TempDays>DaysInMonth(TempMonth)) TempDays=DaysInMonth(TempMonth);
      }
      if (FileName==F("/rangelastweek"))
      {
        TempDays-=7;
        if (TempDays<1)
        {
          TempMonth--;
          if (TempMonth<1) { TempMonth=12; TempYear--; }
          TempDays+=DaysInMonth(TempMonth);
        }
      }
      FileName=String(F("/range"))+String(TempYear)+Int2StrLZ(TempMonth)+Int2StrLZ(TempDays)+"to"+GetDateTimeStr(3);
    }
    LogString(FileName+" ("+server.client().remoteIP().toString()+")..",1);
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, F("text/html"), "");       
    while(myFile.available()) 
    {
      TempLine = myFile.readStringUntil('\n');
      TempLine.trim();     
      if (TempLine==F("\/\/{INSERTDATA}"))
      {
        if (Content.length()>0) server.sendContent(Content);  // flush the buffer
        Content="";
        if (FileName.indexOf("/range")==0)
          InjectRangeData(FileName);
        else
          InjectDataFromFile(FileName);
      }
      else
      {
        if (TempLine==F("<!--INSERTDATEPICKER-->") && FileName.indexOf(F("/range"))==0)  // Format "/range20180319to20180402"
             Content+=GetRangeDatePickerString(FileName.substring(6,10)+"-"+FileName.substring(10,12)+"-"+FileName.substring(12,14),FileName.substring(16,20)+"-"+FileName.substring(20,22)+"-"+FileName.substring(22,24),server.arg(2));
        else
            Content+=TempLine+"\n";
      }
      if (Content.length()>CONTENTBUFFSIZE) server.sendContent(Content);
    }
    myFile.close();
    if (Content.length()>0) server.sendContent(Content);
    server.sendContent("");
    server.client().stop();
    LogString(F("OK\n"),0);
    Serial.printf("Mem:%d\n",FreeMem());
    Serial.printf("%dmS\n",millis()-StartTime);   
  }
  else
    LogString(F("template.htm not found\n"),1);
}

bool SendFile(String FileName, String MIMEType)
{ 
  String TempStr;
  String ClientStr;
  File myFile = SD.open(FileName.c_str(), FILE_READ);
  ClientStr=" ("+server.client().remoteIP().toString()+")";
  if (myFile)
  {
    LogString(FileName+ClientStr+"..",1);
    server.streamFile(myFile,MIMEType.c_str());
    myFile.close();
    LogString("OK\n",0);
    return false;
  }
  TempStr = String(F("File Not Found:"))+ server.uri();
  server.send(404, F("text/plain"), TempStr+"\n");
  LogString(TempStr+ClientStr+"\n",1);
  return true;
}

void InjectDataFromFile(String FileName)
{
  String Line="";
  String TempLine;
  float Value;
  float AvgValue;
  int Split;
  int Count=0;
  File myFile;
  String DateStr;
  bool IsMonth=false;
  int Dp;
  if (FileName=="")  return;
  FileName.replace(".dat",".csv");
  myFile = SD.open(FileName.c_str(), FILE_READ);
  if (myFile) 
  {
    if (FileName.indexOf("m.csv")>0) IsMonth=true;
    if (Units) Dp=3; else Dp=1;
    while(myFile.available())
    {
      TempLine = myFile.readStringUntil('\n');
      Split=TempLine.indexOf(",");
      Value=TempLine.substring(Split+1).toFloat();
      DateStr=TempLine.substring(0,Split);
      if (Units)    Value=Value*TUBECONVERSIONFACTOR;
      if (!Count)   AvgValue=Value;
      if (IsMonth)
      {
        AvgValue=(AvgValue+Value*2)/3;
        Line+="{\"date\":\""+DateStr+"\",\"v1\":"+String(AvgValue,Dp+1)+"},\n";
      }
      else
      {
        AvgValue=(AvgValue*20+Value)/(21);
        if (Count>20) Line+="{\"date\":\""+DateStr+"\",\"v1\":"+String(Value,Dp)+",\"v2\":"+String(AvgValue,Dp)+"},\n";        // output format:  { "date":"2012-08-07 10:23","value":13,"avg":13 },
        else          Line+="{\"date\":\""+DateStr+"\",\"v1\":"+String(Value,Dp)+"},\n";        
      }
      Count++;
      if (Line.length()>CONTENTBUFFSIZE)
      {
        server.sendContent(Line);
        Line="";
      }
    }
    if (DateStr.substring(11,16)!="23:59") Line+="{\"date\":\""+DateStr.substring(0,10)+" 23:59\"},\n"; // Pad graph to end of day   
    if (Line.length()>0) server.sendContent(Line); 
    Line="";
    myFile.close();
    return;
  }
  LogString(F(" Data File not found "),0);
}

void LoggingTick()
{  //  output format: date,value\n
  String FileName;
  String DateStr;
  int i;
  float TempVal;
  if (!(OneSecTick%10)) // Every 10 sec..
  {
     now = rtc.now();
     if (now.minute()!=LastMinute) // every minute.. log the data
     {
       LastMinute=now.minute();
       TempVal=(float)LastCpm * TUBECONVERSIONFACTOR * 1000; // Current averaged reading converted to nSv/Hr
       if (((int)TempVal)>AlarmThreshold && AlarmThreshold>0 && !AlarmLockout)
       {
          TriggerAlarm(String(TempVal/1000,2)+"uSv/Hr ("+String(LastCpm)+"cpm)",10,AlarmLockoutTime); // message, 10 retrys, lockout time (sec)
          LogString(F("Requesting Alarm\n"),1);
       }
       FileName=GetDateTimeStr(3)+".csv"; // YMD.csv
       DateStr=GetDateTimeStr(1); // Y-M-D H:M
       FileName="/Data/"+String(now.year())+"/"+FileName;
       WriteStringToFile(FileName,DateStr+","+String(LastCpm)+"\n");
// SubmitRadReading(LastCpm);       
       if ((LastMinute==0) && (HourCount>600))  // Only do this if there is enough data to get a reasonable average (for when system in newly started)
       {
         if (HourCount>0) HourAvg=((float)60*HourTotal)/(float)HourCount; // Average cpm over the  hour - wont cope with more than 1,200,000 cpm, but thats the limit of the tube anyway
         FileName=GetDateTimeStr(4)+"m.csv"; // YMm.csv
         FileName="/Data/"+String(now.year())+"/"+FileName;
         WriteStringToFile(FileName,DateStr+","+String(HourAvg)+"\n");
         HourCount=0;
         HourTotal=0;
         TempVal=(float)HourAvg * TUBECONVERSIONFACTOR * 1000; // Current averaged reading converted to nSv/Hr
         if (((int)TempVal)>AlarmThresholdHH && AlarmThresholdHH>0 && !AlarmLockout)
         {
            TriggerAlarm(String(TempVal/1000,2)+"uSv/Hr ("+String(HourAvg)+"cpm)",10,AlarmLockoutTime); // message, 10 retrys, lockout time (sec)
            LogString(F("Requesting Alarm (Hour High)\n"),1);
         }
         if (((int)TempVal)<AlarmThresholdHL && AlarmThresholdHL>0 && !AlarmLockout)
         {
            TriggerAlarm(String(TempVal/1000,2)+"uSv/Hr ("+String(HourAvg)+"cpm)",10,AlarmLockoutTime); // message, 10 retrys, lockout time (sec)
            LogString(F("Requesting Alarm (Hour Low)\n"),1);
         }         
       }
     }
     if (LastDay!=now.day())
     {
        LastDay=now.day();
        CreateArchivePage();
     }     
  }
}

void LogIP()
{
  String TempStr=server.client().remoteIP().toString();
  if (TempStr.substring(0,3)!="192")
  {
    File IPFile = SD.open(F("ip.txt"), FILE_WRITE);
    if (IPFile)
    {
      IPFile.print(GetDateTimeStr(0)+","+TempStr+"\n");
      IPFile.close();  
    }
  }
}

void HandleNotFound()
{
    String Uri=server.uri();
    Uri.toLowerCase();
    if (Uri.indexOf("/xml")<0) LogIP();
    if (FreeMem()<8000)
    {
      LogString(String(F("Low Mem:"))+FreeMem()+"\n",1);
      Reboot(true);
    }
    if (Uri.length()<MAXFNLENGTH)
    {
      if (Uri.indexOf("/xml")>=0)    { SendRTData(); return; }      
      if (Uri.indexOf("/range")>=0)  { ShowGraphPage(Uri); return;}
      if (Uri=="/")                  { ShowGraphPage("/rangelastweek"); return;}
      if (Uri=="/today")             { ShowGraphPage("/today"); return;}
      if (Uri=="/favicon.ico")       { SendFile(Uri, F("image/x-icon")); return; }
      if (Uri=="/robots.txt")        { SendFile(Uri, F("text/plain")); return;}
      if (Uri=="/admin.htm")         { HandleAdminPage(); return; }
      if (Uri=="/swaptheunits")      { SwapUnits(); return; }
      if (Uri=="/log.txt")           { SendFile(Uri, F("text/plain")); return;}
      if (Uri=="/ip.txt")            { SendFile(Uri, F("text/plain")); return;}
      if (Uri.indexOf(".dat")>0)     { ShowGraphPage(Uri); return; }
      if (Uri.indexOf(".csv")>0)     { SendFile(Uri, F("text/plain")); return; }
      if (Uri.indexOf(".htm")>0)     { SendFile(Uri, F("text/html")); return; }
      server.send(404, F("text/plain"), String(F("File not found:"))+Uri+"\n");
      LogString(Uri+" ("+server.client().remoteIP().toString()+") Not Found\n",1);
    }
    else
      server.send(400, F("text/plain"), F("Filename too long\n"));
}

int ReadIntArgValue(String Arg,int * VarToSet)
{
  if (server.hasArg(Arg.c_str()))
  {
    *VarToSet=server.arg(Arg.c_str()).toInt();
    return 0;
  }
  return 1;
}

int WifiSignalStrength()
{
  byte AvailableNetworks = WiFi.scanNetworks();
  for (int Network = 0; Network < AvailableNetworks; Network++)
  {
    if (WiFi.SSID(Network) == WiFi.SSID()) return WiFi.RSSI(Network);
  }
  return -1;
}


void HandleAdminPage() // Request for Admin Page
{
  String msg;
  String Content;
  LogString("/admin.htm ("+server.client().remoteIP().toString()+")..",1);
  if (server.hasArg(F("LOGOUT")) || LoginTimeout==1) // Logout requested, or timed out
  {
      LoginTimeout=0;
      server.sendHeader(F("Location"),F("/admin.htm"));
      server.sendHeader(F("Cache-Control"),F("no-cache"));
      server.sendHeader(F("Set-Cookie"),F("RADMON_LOGIN=0"));
      server.send(301);
      LogString(F("OK (Logout)\n"),0);
      return;
  }

  if (IsAuthenticated() && LoginTimeout>0)
  {
    LoginTimeout=300; // Admin inactivity timout = 5 minutes
    if (server.args()>0)
    {
      int Year,Month,Day,Hour,Min,Sec,Err=0;
      Err+=ReadIntArgValue(F("Year"),&Year);
      Err+=ReadIntArgValue(F("Month"),&Month);
      Err+=ReadIntArgValue(F("Day"),&Day);
      Err+=ReadIntArgValue(F("Hour"),&Hour);
      Err+=ReadIntArgValue(F("Minute"),&Min);
      Err+=ReadIntArgValue(F("Second"),&Sec);
      if (!Err) rtc.adjust(DateTime(Year, Month, Day, Hour, Min, Sec));
      if (!ReadIntArgValue(F("PWM"),&PwmWidth)) analogWrite(PWMPIN, PwmWidth); // PWM Pulse width
      ReadIntArgValue(F("AUDIOALARM"),&AudioAlarm);
      ReadIntArgValue(F("AUDIOEN"),&AudioEnabled);
      
      WriteNVR(NVR_AUDIO,AudioEnabled&255);
      WriteNVR(NVR_AUDIOALARM,AudioAlarm&255);

      if (server.hasArg(F("ALARM")))
      {
        ReadIntArgValue(F("ALARM"),&AlarmThreshold);
        WriteNVR(NVR_ALARM,AlarmThreshold/256);
        WriteNVR(NVR_ALARM+1,AlarmThreshold&255);
        AlarmLockout=0;
      }
      if (server.hasArg(F("ALARMHL")))
      {
        ReadIntArgValue(F("ALARMHL"),&AlarmThresholdHL);
        WriteNVR(NVR_ALARM_HR_L,AlarmThresholdHL/256);
        WriteNVR(NVR_ALARM_HR_L+1,AlarmThresholdHL&255);
        AlarmLockout=0;
      }
      if (server.hasArg(F("ALARMHH")))
      {
        ReadIntArgValue(F("ALARMHH"),&AlarmThresholdHH);
        WriteNVR(NVR_ALARM_HR_H,AlarmThresholdHH/256);
        WriteNVR(NVR_ALARM_HR_H+1,AlarmThresholdHH&255);
        AlarmLockout=0;
      }
      if (server.hasArg(F("LOCKOUT")))
      {
        ReadIntArgValue(F("LOCKOUT"),&AlarmLockoutTime);
        AlarmLockoutTime=AlarmLockoutTime*60;
        WriteNVR(NVR_LOCKOUT,AlarmLockoutTime/256);
        WriteNVR(NVR_LOCKOUT+1,AlarmLockoutTime&255);
        AlarmLockout=0;
      }
      if (server.hasArg(F("reboot")))     Reboot(true);
      if (server.hasArg(F("ALARMTEST")))
      {
        TriggerAlarm(F("Test"),1,0); // No retry, no lockout
        ServiceAlarm();
        TriggerAlarm("",0,0); // clear it down in case it failed
      }
      if (server.hasArg(F("CLEARLOG")))
        if (server.arg("CLEARLOG") == AdminPw)
        {
            Serial.println(F("Clearing Log"));
            SD.remove(F("log.txt"));
            SD.remove(F("ip.txt"));
            LogString(String(F("Logs Deleted ("))+server.client().remoteIP().toString()+")..",1);
        }
    }
    now = rtc.now();
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, F("text/html"), "");   
    Content = String(NavBar)+String(CcsStyle);
    server.sendContent(Content);
    Content = "";
    Content += F("<div class='panel panel-default'><div class='panel-heading'>Set Time/Date</div><div class='panel-body'><form action='/admin.htm' method='POST'>");
    Content += String(F("<p><input type='text' size=4 name='Year' value='"))+String(now.year())+String(F("'> Year<br>"));
    Content += String(F("<input type='text' size=4 name='Month' value='"))+String(now.month())+String(F("'> Month<br>"));
    Content += String(F("<input type='text' size=4 name='Day' value='"))+String(now.day())+String(F("'> Day<br>"));
    Content += String(F("<input type='text' size=4 name='Hour' value='"))+String(now.hour())+String(F("'> Hour<br>"));
    Content += String(F("<input type='text' size=4 name='Minute' value='"))+String(now.minute())+String(F("'> Minute<br>"));
    Content += String(F("<input type='text' size=4 name='Second' value='"))+String(now.second())+String(F("'> Second<br>"));
    Content += F("<input type='submit' size=4 name='SUBMIT' value='Submit'></p></form><br></div></div>");
    Content += F("<div class='panel panel-default'><div class='panel-heading'>Setup</div><div class='panel-body'><form action='/admin.htm' method='POST'>");
    Content += String(F("<p><input type='text' size=4 name='PWM' value='"))+String(PwmWidth)+String(F("'> PWM Pulse Width<br>"));
    Content += String(F("<input type='text' size=4 name='ALARM' value='"))+String(AlarmThreshold)+String(F("'> Alert Alarm Threshold (nSv/Hr) 0=No Alarm<br>"));
    Content += String(F("<input type='text' size=4 name='ALARMHL' value='"))+String(AlarmThresholdHL)+String(F("'> Hour Average Alert Alarm Threshold - LOW (nSv/Hr) 0=No Alarm<br>"));
    Content += String(F("<input type='text' size=4 name='ALARMHH' value='"))+String(AlarmThresholdHH)+String(F("'> Hour Average Alert Alarm Threshold - HIGH (nSv/Hr) 0=No Alarm<br>"));  
    Content += String(F("<input type='text' size=4 name='LOCKOUT' value='"))+String(int(AlarmLockoutTime/60))+String(F("'> Alert Alarm Lockout period (min)<br>"));   
    if (AudioEnabled) Content += F("Audio Click: <input type='radio' name='AUDIOEN' value='1' checked>&nbsp On <input type='radio' name='AUDIOEN' value='0'>&nbsp Off<br>");
    else              Content += F("Audio Click: <input type='radio' name='AUDIOEN' value='1' >&nbsp On <input type='radio' name='AUDIOEN' value='0'checked>&nbsp Off<br>");
    if (AudioAlarm)   Content += F("Audio Alarm: <input type='radio' name='AUDIOALARM' value='1' checked>&nbsp On <input type='radio' name='AUDIOALARM' value='0'>&nbsp Off<br>");
    else              Content += F("Audio Alarm: <input type='radio' name='AUDIOALARM' value='1' >&nbspOn <input type='radio' name='AUDIOALARM' value='0'checked>&nbsp Off<br>");
    Content += F("<input type='submit' size=4 name='SUBMIT' value='Submit'></p></form><br>");
    Content += F("<form action='/log.txt' method=\'post'><p><input type='submit' name='SHOWLOG' value='Show Log'></p></form>");
    Content += F("<form action='/ip.txt' method=\'post'><p><input type='submit' name='SHOWIPLOG' value='Show IP Log'></p></form>");
    Content += F("<form action='/admin.htm' method='POST'><p>");
    Content += F("<input type='submit' name='SUBMIT' value='Clear Logs'>");
    Content += F("<input type='password' name='CLEARLOG' placeholder='password'></p></form>");
    Content += F("<form action='/admin.htm' method='post'><p><input type='submit' name='ALARMTEST' value='Alarm Test'></p></form>");
    if (AlarmStatus==ALARM_FAILMEM) Content += F("<p>Send Failed (Low memory)</p>");
    if (AlarmStatus==ALARM_FAIL) Content += F("<p>Send Failed</p>");
    if (AlarmStatus==ALARM_OK) Content += F("<p>Send OK</p>");
    AlarmStatus=0;
    Content += F("<form action='/admin.htm' method='post'><p><input type='submit' name='reboot' value='Reboot'></p></form>");
    Content += F("</div></div><p>");
    Content += String(F("Free Memory: ")) + String(FreeMem())+"<br>";
    Content += String(F("Build: "))+String(CompileDate)+"<br>";
    Content += String(F("Up Time: ")) + String(OneSecTick/3600)+"Hr "+ String((OneSecTick/60)%60)+"Min <br>";
    Content += String(F("Average count since boot: "))+String((float)(TotCount*60)/OneSecTick)+"<br>";
    server.sendContent(Content);
    Content="";
    Content += String(F("Wifi Signal Strength: "))+String(WifiSignalStrength())+"dBm ("+String(WiFi.SSID())+")<br></p>";
    Content += F("<form action='/admin.htm' method='post'><p><input type='submit' name='LOGOUT' value='Log Out'></p>");
    Content += F("</body></html>");
    server.sendContent(Content);
    server.sendContent("");
    server.client().stop();
    LogString(F("Admin Page Complete\n"),0);
  }
  else
  {
    if (server.hasArg(F("PASSWORD"))) // Login requested
    {
      if (server.arg(F("PASSWORD")) == AdminPw )
      {
        LoginTimeout=300;
        server.sendHeader(F("Location"),F("/admin.htm")); // Redirect back here, this time logged on
        server.sendHeader(F("Cache-Control"),F("no-cache"));
        server.sendHeader(F("Set-Cookie"),F("RADMON_LOGIN=1"));
        server.send(303);
        LogString(F("OK (Logged in)\n"),0);
        return;
      }
      msg = F("<p>Wrong password!</p>");
      LogString(F("Login Fail "),0);
      server.sendHeader(F("Set-Cookie"),F("RADMON_LOGIN=0"));
    }
    Content = String(NavBar)+String(CcsStyle);
    Content += F("<br><form action='/admin.htm' method='POST'><p>");
    Content += F("Password: <input type='password' name='PASSWORD' placeholder='password'> ");
    Content += String(F("<input type='submit' name='SUBMIT' value='Submit'></p></form>")) + msg + String(F("</body></html>"));
    server.send(200, F("text/html"), Content);
    LogString(F("OK (Login Page)\n"),0);
  }
}

bool IsAuthenticated()
{
  if (server.hasHeader(F("Cookie")))
  {
    String cookie = server.header(F("Cookie"));
    if (cookie.indexOf(F("RADMON_LOGIN=1")) != -1)  return true;
  }
  return false;
}

/* int WriteUnitsPref(int Units)
{
  server.sendHeader("Set-Cookie","RADMON_UNITS="+String(Units));  
}

int ReadUnitsPref()
{
  int Units;
  if (server.hasHeader(F("Cookie")))
  {
    String cookie = server.header(F("Cookie"));
    if (cookie.indexOf(F("RADMON_UNITS")) != -1)
    {
      if (cookie.indexOf(F("RADMON_UNITS=0")) != -1) Units=0;
      if (cookie.indexOf(F("RADMON_UNITS=1")) != -1) Units=1;
      return Units;
    }
  }
  WriteUnitsPref(0);
  return 0;
} */


void InjectRangeData(String Range) // Range Format: "/rangeyyyymmddtoyyyymmdd" for hour average or "/rangeyyyymmddtoyyyymmddday" for day average
{ // Builds a graph based on hourly readings. Uses the 'month' files
    unsigned long MonthFrom, MonthTo, YearFrom, YearTo;
    String FileName;
    String Line;
    String Content="";
    String FromDateStr;
    String ToDateStr;
    String DateStr;
    String LastDate="";
    float Value;
    float DayTotal=0;
    int DayCount=0;
    bool UseDayAverage=false;
    File MonthFile;
    if (Range.indexOf("day")>=0)  UseDayAverage=true;
    FromDateStr=Range.substring(6,14);
    ToDateStr=Range.substring(16,24);
    YearFrom=FromDateStr.substring(0,4).toInt();
    MonthFrom=FromDateStr.substring(4,6).toInt();
    YearTo=ToDateStr.substring(0,4).toInt();
    MonthTo=ToDateStr.substring(4,6).toInt();
    if (YearFrom<2018) YearFrom=2018;
    if (FromDateStr>ToDateStr || MonthFrom>12 || MonthTo>12) return;
    do
    {
      FileName="/Data/"+String(YearFrom)+"/"+String(YearFrom)+Int2StrLZ(MonthFrom)+"m.csv";      
      if (SD.exists(FileName))
      {
        MonthFile= SD.open(FileName, FILE_READ);
        LastDate="";
        while(MonthFile.available())
        {
          Line = MonthFile.readStringUntil('\n'); // Format 
          if (LastDate=="") LastDate=Line.substring(0,10);
          Value=Line.substring(Line.indexOf(",")+1).toFloat();
          if (Units) Value=Value*TUBECONVERSIONFACTOR;
          if (UseDayAverage==false)
          {
              DateStr=Line.substring(0,4)+Line.substring(5,7)+Line.substring(8,10); // Line Format yyyy-mm-dd Date format yyyymmdd
              if (DateStr>=FromDateStr && DateStr<=ToDateStr) Content+="{\"date\":\""+Line.substring(0,16)+"\",\"v1\":"+String(Value,3)+"},\n";
          }
          else
          {
              if (Line.substring(0,10)!=LastDate || !MonthFile.available())
              {
                if (!MonthFile.available() && Line.substring(0,10)==LastDate) // Catch the edge case where this is the last value in the file & the filter runs to the end
                {
                  DayTotal+=Value;
                  DayCount++;                  
                }
                DateStr=LastDate.substring(0,4)+LastDate.substring(5,7)+LastDate.substring(8,10); // Line Format yyyy-mm-dd Date format yyyymmdd
                if (DateStr>=FromDateStr && DateStr<=ToDateStr) Content+="{\"date\":\""+LastDate+"\",\"v1\":"+String(DayTotal/DayCount,3)+"},\n";
                DayTotal=0;
                DayCount=0;
              }
              DayTotal+=Value;
              DayCount++;
          }
          if (Content.length()>CONTENTBUFFSIZE)
          {
             server.sendContent(Content);
             Content="";
          }
          LastDate=Line.substring(0,10);
        }
        MonthFile.close();        
      }      
      MonthFrom++;
      if (MonthFrom>12)
      {
        MonthFrom=1;
        YearFrom++;
      }
    }
    while (YearFrom*12+MonthFrom <= YearTo*12+MonthTo);
    if (Content.length()>0) server.sendContent(Content);          
}

void Reboot(bool Manual)
{
  LogString(F("\n"),0);
  if (Manual) LogString(F("Rebooting..\n"),1);
  else        LogString(F("WD Reset..\n"),1);
  ESP.restart();
}

void WriteNVR(byte Loc,byte Val)
{
  if (rtc.readnvram(Loc)!=Val)  rtc.writenvram(Loc,Val);
}

byte ReadNVR(byte Loc)
{
  return rtc.readnvram(Loc);
}

String Int2StrLZ(int Val) // 0..99 only
{
  if (Val>9) return String(Val);
  else       return "0"+String(Val);
}

void WriteStringToFile(String FileName,String Str)
{
  File TempFile;
  String Path;
  int Temp=FileName.lastIndexOf("/"); 
  if (Temp>=0)
  {
    Path=FileName.substring(0,Temp);
    if (!SD.exists(Path)) SD.mkdir(Path);
  }
  TempFile = SD.open(FileName, FILE_WRITE);
  if (TempFile)
  {
    TempFile.print(Str);
    TempFile.close();  
  }  
}

void CreateArchivePage()
{
    int MaxYear=0;
    int Month;
    int Day;
    String FileName;    
    int FileCount=0;
    String Content;
    String YearStr;
    String DayStr;
    File OptFile;
    File dir=SD.open(F("/Data"));
    if (dir)
    {
      OptFile = SD.open(F("/archive.htm"), FILE_READ);
      Content = OptFile.readStringUntil('\n'); // Format "<!--20181225-->"
      if (Content.substring(4,8).toInt()==now.year() && Content.substring(8,10).toInt()==now.month() && Content.substring(10,12).toInt()==now.day()) return;     
      OptFile.close();
      Serial.print(F("Creating Archive Page.."));
      SD.remove(F("/archive.htm"));
      OptFile = SD.open(F("/archive.htm"), FILE_WRITE);
      OptFile.print("<!--"+String(now.year())+Int2StrLZ(now.month())+Int2StrLZ(now.day())+"-->\n");
      OptFile.print(String(NavBar)+"\n"+String(CcsStyle)+"\n");      
      OptFile.print(F("<div class='container'>"));
      
      dir.rewindDirectory();
      while(true)
      { 
        File entry =  dir.openNextFile();
        if (!entry) break;
        if (entry.isDirectory())
        {
          FileCount++;
          FileName=entry.name();
          if (FileName.toInt()>MaxYear) MaxYear=FileName.toInt();
        }
        entry.close();
      }
      dir.close();
      
      while (FileCount)
      {
        YearStr=String(MaxYear);
        if (MaxYear>2017 && MaxYear<9999 && SD.exists("/Data/"+YearStr))
        {
          FileCount--;
          OptFile.print(F("<div class='panel panel-default'>"));
          OptFile.print(String(F("<div class='well well-sm'><b><a href='/range"))+YearStr+"0101to"+YearStr+"1231'>"+YearStr+"</a></b></div>");
          for (Month=12;Month>0;Month--)
          {
            FileName="/Data/"+YearStr+"/"+YearStr+Int2StrLZ(Month)+"m.csv";
            if (SD.exists(FileName))
            {
              FileName.replace(".csv",".dat");
              OptFile.print(F("<div class='row'><div class='panel-group'><div class='col-sm-1'></div>"));
              OptFile.print(String(F("<div class='col-sm-2'><div class='well well-sm'><a href='"))+FileName+"'>"+MonthStr(Month)+"</a></div></div>\n");              
              OptFile.print(String(F("<div class='col-sm-2'><div class='panel panel-default'><div class='panel-heading'><a data-toggle='collapse' href='#collapse"))+String(Month)+String(F("'>Show days</a></div><div id='collapse"))+String(Month)+String(F("' class='panel-collapse collapse'><ul class='list-group'>")));
              FileName="/Data/"+YearStr+"/"+YearStr+Int2StrLZ(Month); // Format: "/2018/02/201802"
              for (Day=31;Day>0;Day--)
              {
                DayStr=Int2StrLZ(Day);
                if (SD.exists(FileName+DayStr+".csv"))
                {
                  OptFile.print(String(F("<li class='list-group-item'><a href='"))+FileName+DayStr+".dat'>"+DayStr+"</a></li>\n");
                }
              }
              OptFile.print(F("</ul></div></div></div></div></div>\n"));
            }
          }
        }
        MaxYear--;
      }
      OptFile.print(F("</div></div></body></html>"));
      OptFile.close();
    }
    Serial.print(F("Done\n"));
}

String GetRangeDatePickerString(String FromDate, String ToDate, String DayAv)
{ // Construct the string required to add a date picker to a page
  if (DayAv==F("day")) DayAv=F("checked"); 
  return String(F("<div style='margin-left: 40px'><form class='form-inline' action='/range' method='post'><div class='form-group'><label for='fromdate'>From:</label><input type='date' class='form-control' id='fromdate' name='from' value='"))+FromDate+String(F("'>&nbsp</div><div class='form-group'><label for='todate'>To:</label><input type='date' class='form-control' id='todate' name='to' value='"))+ToDate+String(F("'>&nbsp</div><label class='checkbox-inline'><input type='checkbox' name='day' value='day'"))+DayAv+String(F(">Day Average </label>&nbsp<button type='submit' class='btn btn-default'>Submit</button></form></div>"));
}

void SubmitRadReading(int CPM)
{ // GET /radmon.php?function=submit&user=UserName&password=PassWord&value=CPM&unit=CPM HTTP/1.0 HOST: radmon.org
  WiFiClient client;  
  String Temp;
  String Username="blah";
  String Password="blah";
  Temp="GET /radmon.php?function=submit&user="+Username+"&password="+Password+"&value="+String(CPM)+"&unit=CPM HTTP/1.0 HOST: radmon.org Connection: close\r\n\r\n";
  Serial.println("Subitting reading");
  client.println(Temp);
  Serial.println("Respond:");
  while(client.available())
  {
    String line = client.readStringUntil('\r');
    Serial.print(line);
  }
}

void setup()
{
  pinMode(AUDIOPIN, OUTPUT); // LED
  Beep(100);
  attachInterrupt(INTPIN, PulseISR, FALLING);  
  pinMode(INTPIN, INPUT);
  PwmWidth=300;
  analogWrite(PWMPIN, PwmWidth); // PWM Pulse width
  analogWriteFreq(8000);  // PWM Frequency
  Serial.begin(74880); 
  delay(100);
  Serial.println();
  InitSDCard();
  LogString(F("=========BOOT!========\n"),0);  
  InitRTC();
  InitSystem();
  InitTimers();
  InitWifi();  // If there were no problems connecting last time
  InitWebServer();
  now = rtc.now();
  LastMinute=now.minute();
  LastDay=now.day();
  Serial.printf("Mem:%d\n",FreeMem());
  AlarmThresholdHL=ReadNVR(NVR_ALARM_HR_L)*256+ReadNVR(NVR_ALARM_HR_L+1);
  AlarmThresholdHH=ReadNVR(NVR_ALARM_HR_H)*256+ReadNVR(NVR_ALARM_HR_H+1);  
  AlarmThreshold=ReadNVR(NVR_ALARM)*256+ReadNVR(NVR_ALARM+1);
  AlarmLockoutTime=ReadNVR(NVR_LOCKOUT)*256+ReadNVR(NVR_LOCKOUT+1);
  AudioEnabled=ReadNVR(NVR_AUDIO);
  AudioAlarm=ReadNVR(NVR_AUDIOALARM);
//  SD.remove("/archive.htm");
  CreateArchivePage();
}

void  CheckWifi() // if no wifi, will wait for a while & then reboot. This reboot will try to connect to wifi right away
{
  int Temp;
  byte DelayCount;
  if (WiFi.status() != WL_CONNECTED)
  {
    Beep(30);
    DelayCount=ReadNVR(NVR_DELAY); // How long should we wait before giving up & rebooting?
    Temp=1;
    for (int i=0;i<DelayCount;i++) Temp=Temp*4; // Timeouts after 4,16,64,256,1024 ... etc minutes
    Temp=Temp*60; // convert to seconds
    if ((OneSecTick-LastGoodWifi)>Temp && Temp<=(1024*60))
    {
      LogString(F("Wifi Loss Timeout.. rebooting\n"),1);    
      WriteNVR(NVR_TIMEOUT,0); // Try to boot normally again with auto connect to wifi
      Reboot(false);
    }
  }
  else
    LastGoodWifi=OneSecTick;
}


void loop()
{
  if (OneSecFlag)
  {
    OneSecFlag=false;
    Watchdog=60;
    if (OneSecTick>60)      LoggingTick();
    if (!(OneSecTick%60))   CheckWifi(); // Every minute check the wifi
    if (AudioAlarmCount>0)
    {
       Beep(200);
       AudioAlarmCount--;
    }  
    ServiceAlarm();
    AlarmStatus=0; // stops result showing up on the admin page
  }
  server.handleClient();
}

