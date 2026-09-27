/*
   ============================================================
        SISTEM ABSENSI SEKOLAH BERBASIS IoT - ESP32 V3
   ============================================================

   Hardware:
   - ESP32
   - Fingerprint AS608
   - LCD I2C 16x2
   - Buzzer
   - LED Hijau
   - LED Merah

   Fitur:
   - Access Point
   - WiFi STA untuk NTP
   - NTP WIB UTC+7
   - Tambah siswa
   - Hapus siswa
   - Enroll fingerprint
   - Absensi fingerprint
   - Hadir / Terlambat
   - Rekap harian
   - Rekap per siswa
   - Rekap bulanan
   - Pengaturan jam masuk
   - Penyimpanan LittleFS

   PIN:
   AS608 TX -> GPIO 16
   AS608 RX -> GPIO 17
   LCD SDA  -> GPIO 21
   LCD SCL  -> GPIO 22
   Buzzer   -> GPIO 25
   LED Hijau-> GPIO 26
   LED Merah-> GPIO 27
*/

// ============================================================
// LIBRARY
// ============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Fingerprint.h>
#include <time.h>

// ============================================================
// PIN
// ============================================================

#define FP_RX 16
#define FP_TX 17

#define BUZZER_PIN 25
#define LED_GREEN 26
#define LED_RED 27

// ============================================================
// WIFI
// ============================================================

// Access Point ESP32
const char* AP_SSID = "ABSENSI_ESP";
const char* AP_PASSWORD = "12345678";

// WiFi sekolah untuk NTP
// Isi sesuai WiFi sekolah.
// Jika tidak ingin memakai STA, biarkan kosong.
const char* WIFI_SSID = "vivo";
const char* WIFI_PASSWORD = "qibtiyah";

// ============================================================
// NTP
// ============================================================

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";

// WIB = UTC+7
const long GMT_OFFSET_SEC = 7 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

// ============================================================
// DEFAULT JAM MASUK
// ============================================================

int jamMasuk = 6;
int menitMasuk = 30;

// ============================================================
// FILE LITTLEFS
// ============================================================

#define FILE_STUDENTS "/students.json"
#define FILE_SETTINGS "/settings.json"
#define FILE_ATTENDANCE "/attendance.csv"

// ============================================================
// OBJECT
// ============================================================

HardwareSerial FingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&FingerSerial);

LiquidCrystal_I2C lcd(0x27, 16, 2);

WebServer server(80);

// ============================================================
// DATA SISWA
// ============================================================

struct Student {
  int id;
  String name;
};

const int MAX_STUDENTS = 100;

Student students[MAX_STUDENTS];
int studentCount = 0;

// ============================================================
// STATUS ENROLL
// ============================================================

bool enrollmentRunning = false;
int enrollmentID = -1;
String enrollmentName = "";

// ============================================================
// HELPER
// ============================================================

String twoDigit(int value) {
  if (value < 10) return "0" + String(value);
  return String(value);
}

String getDate() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return "0000-00-00";
  }

  return String(timeinfo.tm_year + 1900) + "-" +
         twoDigit(timeinfo.tm_mon + 1) + "-" +
         twoDigit(timeinfo.tm_mday);
}

String getTimeNow() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return "00:00:00";
  }

  return twoDigit(timeinfo.tm_hour) + ":" +
         twoDigit(timeinfo.tm_min) + ":" +
         twoDigit(timeinfo.tm_sec);
}

String getMonth() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return "0000-00";
  }

  return String(timeinfo.tm_year + 1900) + "-" +
         twoDigit(timeinfo.tm_mon + 1);
}

bool isTimeLate() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    return false;
  }

  int currentMinutes =
      timeinfo.tm_hour * 60 + timeinfo.tm_min;

  int limitMinutes =
      jamMasuk * 60 + menitMasuk;

  return currentMinutes > limitMinutes;
}

String getAttendanceStatus() {
  if (isTimeLate()) {
    return "Terlambat";
  }

  return "Hadir";
}

int findStudentByID(int id) {

  for (int i = 0; i < studentCount; i++) {

    if (students[i].id == id) {
      return i;
    }
  }

  return -1;
}

String jsonEscape(String input) {

  input.replace("\\", "\\\\");
  input.replace("\"", "\\\"");
  input.replace("\n", "\\n");
  input.replace("\r", "");

  return input;
}

// ============================================================
// LCD
// ============================================================

void lcdMessage(String line1, String line2) {

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));

  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

// ============================================================
// LED / BUZZER
// ============================================================

void successSignal() {

  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_RED, LOW);

  tone(BUZZER_PIN, 2000, 150);

  delay(700);

  digitalWrite(LED_GREEN, LOW);
}

void failedSignal() {

  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, HIGH);

  tone(BUZZER_PIN, 500, 400);

  delay(700);

  digitalWrite(LED_RED, LOW);
}

// ============================================================
// SETTINGS
// ============================================================

void loadSettings() {

  if (!LittleFS.exists(FILE_SETTINGS)) {
    return;
  }

  File file = LittleFS.open(FILE_SETTINGS, "r");

  if (!file) return;

  DynamicJsonDocument doc(1024);

  DeserializationError error =
      deserializeJson(doc, file);

  file.close();

  if (!error) {

    jamMasuk =
        doc["jamMasuk"] | 6;

    menitMasuk =
        doc["menitMasuk"] | 30;
  }
}

void saveSettings() {

  DynamicJsonDocument doc(1024);

  doc["jamMasuk"] = jamMasuk;
  doc["menitMasuk"] = menitMasuk;

  File file =
      LittleFS.open(FILE_SETTINGS, "w");

  if (!file) return;

  serializeJson(doc, file);

  file.close();
}

// ============================================================
// STUDENT DATABASE
// ============================================================

void loadStudents() {

  studentCount = 0;

  if (!LittleFS.exists(FILE_STUDENTS)) {
    return;
  }

  File file =
      LittleFS.open(FILE_STUDENTS, "r");

  if (!file) return;

  DynamicJsonDocument doc(10000);

  DeserializationError error =
      deserializeJson(doc, file);

  file.close();

  if (error) {
    return;
  }

  JsonArray arr = doc.as<JsonArray>();

  for (JsonObject obj : arr) {

    if (studentCount >= MAX_STUDENTS) {
      break;
    }

    students[studentCount].id =
        obj["id"] | 0;

    students[studentCount].name =
        String(obj["name"] | "");

    studentCount++;
  }
}

void saveStudents() {

  DynamicJsonDocument doc(12000);

  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < studentCount; i++) {

    JsonObject obj = arr.createNestedObject();

    obj["id"] = students[i].id;
    obj["name"] = students[i].name;
  }

  File file =
      LittleFS.open(FILE_STUDENTS, "w");

  if (!file) return;

  serializeJson(doc, file);

  file.close();
}

// ============================================================
// ATTENDANCE DATABASE
// ============================================================

void createAttendanceFile() {

  if (!LittleFS.exists(FILE_ATTENDANCE)) {

    File file =
        LittleFS.open(FILE_ATTENDANCE, "w");

    if (!file) return;

    file.println("date,time,id,name,status");

    file.close();
  }
}

// ============================================================
// CEK APAKAH SUDAH ABSEN HARI INI
// ============================================================

bool alreadyAttendanceToday(int id) {

  if (!LittleFS.exists(FILE_ATTENDANCE)) {
    return false;
  }

  String today = getDate();

  File file =
      LittleFS.open(FILE_ATTENDANCE, "r");

  if (!file) return false;

  while (file.available()) {

    String line = file.readStringUntil('\n');

    line.trim();

    if (line.length() == 0) {
      continue;
    }

    // format:
    // date,time,id,name,status

    int p1 = line.indexOf(',');

    if (p1 < 0) continue;

    String date =
        line.substring(0, p1);

    int p2 =
        line.indexOf(',', p1 + 1);

    if (p2 < 0) continue;

    int p3 =
        line.indexOf(',', p2 + 1);

    if (p3 < 0) continue;

    String idString =
        line.substring(p2 + 1, p3);

    if (date == today &&
        idString.toInt() == id) {

      file.close();

      return true;
    }
  }

  file.close();

  return false;
}

// ============================================================
// SIMPAN ABSENSI
// ============================================================

void saveAttendance(
    int id,
    String name,
    String status) {

  File file =
      LittleFS.open(FILE_ATTENDANCE, "a");

  if (!file) {
    return;
  }

  file.print(getDate());
  file.print(",");

  file.print(getTimeNow());
  file.print(",");

  file.print(id);
  file.print(",");

  // koma dalam nama diubah menjadi spasi
  name.replace(",", " ");

  file.print(name);
  file.print(",");

  file.println(status);

  file.close();
}

// ============================================================
// FINGERPRINT SEARCH
// ============================================================

int getFingerprintID() {

  uint8_t p =
      finger.getImage();

  if (p != FINGERPRINT_OK) {
    return -1;
  }

  p =
      finger.image2Tz();

  if (p != FINGERPRINT_OK) {
    return -1;
  }

  p =
      finger.fingerFastSearch();

  if (p != FINGERPRINT_OK) {
    return -1;
  }

  return finger.fingerID;
}

// ============================================================
// ABSENSI
// ============================================================

void processFingerprint() {

  int id = getFingerprintID();

  if (id < 0) {
    return;
  }

  int index =
      findStudentByID(id);

  if (index < 0) {

    lcdMessage(
        "ID Tidak Terdaftar",
        "ID: " + String(id));

    failedSignal();

    delay(1000);

    lcdMessage(
        "Tempelkan Jari",
        "Silakan Absen");

    return;
  }

  String name =
      students[index].name;

  lcdMessage(
      "Halo!",
      name);

  if (alreadyAttendanceToday(id)) {

    lcdMessage(
        "Sudah Absen",
        name);

    tone(BUZZER_PIN, 1000, 300);

    delay(1500);

    lcdMessage(
        "Tempelkan Jari",
        "Silakan Absen");

    return;
  }

  String status =
      getAttendanceStatus();

  saveAttendance(
      id,
      name,
      status);

  if (status == "Hadir") {

    lcdMessage(
        "ABSEN BERHASIL",
        "Hadir " + getTimeNow());

    successSignal();

  } else {

    lcdMessage(
        "ABSEN BERHASIL",
        "Terlambat");

    successSignal();
  }

  delay(1500);

  lcdMessage(
      "Tempelkan Jari",
      "Silakan Absen");
}

// ============================================================
// ENROLL FINGERPRINT
// ============================================================

bool enrollFingerprint(int id) {

  lcdMessage(
      "Daftar Finger",
      "ID: " + String(id));

  delay(500);

  int p = -1;

  // -----------------------------
  // Jari pertama
  // -----------------------------

  lcdMessage(
      "Tempelkan Jari",
      "Pertama");

  while (p != FINGERPRINT_OK) {

    p = finger.getImage();

    if (p == FINGERPRINT_OK) {
      break;
    }

    delay(100);
  }

  p = finger.image2Tz(1);

  if (p != FINGERPRINT_OK) {

    lcdMessage(
        "Fingerprint",
        "Gagal");

    return false;
  }

  lcdMessage(
      "Angkat Jari",
      "Tunggu...");

  delay(1500);

  while (finger.getImage() !=
         FINGERPRINT_NOFINGER) {

    delay(100);
  }

  // -----------------------------
  // Jari kedua
  // -----------------------------

  lcdMessage(
      "Tempelkan Lagi",
      "Jari yang Sama");

  p = -1;

  while (p != FINGERPRINT_OK) {

    p = finger.getImage();

    if (p == FINGERPRINT_OK) {
      break;
    }

    delay(100);
  }

  p = finger.image2Tz(2);

  if (p != FINGERPRINT_OK) {

    lcdMessage(
        "Fingerprint",
        "Gagal");

    return false;
  }

  // -----------------------------
  // Buat model
  // -----------------------------

  p =
      finger.createModel();

  if (p != FINGERPRINT_OK) {

    lcdMessage(
        "Jari Tidak Cocok",
        "Ulangi");

    return false;
  }

  p =
      finger.storeModel(id);

  if (p != FINGERPRINT_OK) {

    lcdMessage(
        "Gagal Simpan",
        "Fingerprint");

    return false;
  }

  lcdMessage(
      "Berhasil!",
      "ID " + String(id));

  delay(1500);

  return true;
}

// ============================================================
// WEB HTML
// ============================================================

const char MAIN_HTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>
<html lang="id">

<head>

<meta charset="UTF-8">

<meta name="viewport"
content="width=device-width, initial-scale=1.0">

<title>Absensi Sekolah</title>

<style>

*{
box-sizing:border-box;
}

body{
margin:0;
font-family:Arial,Helvetica,sans-serif;
background:#f1f5f9;
color:#1e293b;
}

.sidebar{
position:fixed;
left:0;
top:0;
bottom:0;
width:230px;
background:#0f172a;
color:white;
padding:20px;
}

.logo{
font-size:20px;
font-weight:bold;
margin-bottom:25px;
}

.menu{
padding:12px;
margin:5px 0;
border-radius:8px;
cursor:pointer;
}

.menu:hover,
.menu.active{
background:#2563eb;
}

.main{
margin-left:230px;
padding:25px;
}

.header{
display:flex;
justify-content:space-between;
align-items:center;
margin-bottom:20px;
}

.header h1{
margin:0;
}

.cards{
display:grid;
grid-template-columns:
repeat(4,1fr);
gap:15px;
margin-bottom:20px;
}

.card{
background:white;
border-radius:12px;
padding:20px;
box-shadow:
0 2px 8px rgba(0,0,0,.06);
}

.card-title{
color:#64748b;
font-size:14px;
}

.card-value{
font-size:30px;
font-weight:bold;
margin-top:10px;
}

.green{
color:#16a34a;
}

.orange{
color:#ea580c;
}

.red{
color:#dc2626;
}

.blue{
color:#2563eb;
}

.panel{
background:white;
border-radius:12px;
padding:20px;
margin-bottom:20px;
box-shadow:
0 2px 8px rgba(0,0,0,.06);
}

table{
width:100%;
border-collapse:collapse;
}

th,td{
padding:12px;
border-bottom:
1px solid #e2e8f0;
text-align:left;
}

th{
background:#f8fafc;
}

.badge{
padding:5px 9px;
border-radius:20px;
font-size:12px;
font-weight:bold;
}

.badge-hadir{
background:#dcfce7;
color:#166534;
}

.badge-terlambat{
background:#ffedd5;
color:#9a3412;
}

.badge-tidak{
background:#fee2e2;
color:#991b1b;
}

button{
border:0;
padding:10px 15px;
border-radius:7px;
cursor:pointer;
background:#2563eb;
color:white;
}

button.danger{
background:#dc2626;
}

button.green-btn{
background:#16a34a;
}

input,select{
padding:10px;
border:1px solid #cbd5e1;
border-radius:7px;
width:100%;
margin:5px 0 12px;
}

.form-grid{
display:grid;
grid-template-columns:
repeat(2,1fr);
gap:15px;
}

.section{
display:none;
}

.section.active{
display:block;
}

@media(max-width:800px){

.sidebar{
position:relative;
width:100%;
height:auto;
}

.main{
margin-left:0;
padding:15px;
}

.cards{
grid-template-columns:
repeat(2,1fr);
}

.form-grid{
grid-template-columns:1fr;
}

}

</style>

</head>

<body>

<div class="sidebar">

<div class="logo">
&#127979; ABSENSI SEKOLAH
</div>

<div class="menu active"
onclick="showSection('dashboard',this)">
&#127968; Dashboard
</div>

<div class="menu"
onclick="showSection('students',this)">
&#128104;&#8205;&#127891; Data Siswa
</div>

<div class="menu"
onclick="showSection('studentReport',this)">
&#128202; Rekap Siswa
</div>

<div class="menu"
onclick="showSection('monthly',this)">
&#128197; Rekap Bulanan
</div>

<div class="menu"
onclick="showSection('settings',this)">
&#9881;&#65039; Pengaturan
</div>

</div>

<div class="main">

<!-- DASHBOARD -->

<div id="dashboard"
class="section active">

<div class="header">

<div>
<h1>Dashboard</h1>
<p id="today"></p>
</div>

</div>

<div class="cards">

<div class="card">
<div class="card-title">
Total Siswa
</div>
<div class="card-value blue"
id="totalStudents">
0
</div>
</div>

<div class="card">
<div class="card-title">
Hadir
</div>
<div class="card-value green"
id="totalPresent">
0
</div>
</div>

<div class="card">
<div class="card-title">
Terlambat
</div>
<div class="card-value orange"
id="totalLate">
0
</div>
</div>

<div class="card">
<div class="card-title">
Tidak Hadir
</div>
<div class="card-value red"
id="totalAbsent">
0
</div>
</div>

</div>

<div class="panel">

<h2>Absensi Hari Ini</h2>

<table>

<thead>

<tr>
<th>No</th>
<th>Nama</th>
<th>Jam</th>
<th>Status</th>
</tr>

</thead>

<tbody id="todayTable">
</tbody>

</table>

</div>

</div>

<!-- DATA SISWA -->

<div id="students"
class="section">

<div class="panel">

<h2>Tambah Siswa</h2>

<div class="form-grid">

<div>

<label>Nama Siswa</label>

<input
id="studentName"
placeholder="Nama lengkap">

</div>

<div>

<label>ID Fingerprint</label>

<input
id="studentID"
type="number"
placeholder="Contoh: 1">

</div>

</div>

<button
class="green-btn"
onclick="addStudent()">

Tambah & Daftar Fingerprint

</button>

</div>

<div class="panel">

<h2>Data Siswa</h2>

<table>

<thead>

<tr>
<th>ID</th>
<th>Nama</th>
<th>Aksi</th>
</tr>

</thead>

<tbody id="studentTable">
</tbody>

</table>

</div>

</div>

<!-- REKAP SISWA -->

<div id="studentReport"
class="section">

<div class="panel">

<h2>Rekap Per Siswa</h2>

<label>Pilih Siswa</label>

<select id="reportStudent"
onchange="loadStudentReport()">
</select>

<div id="studentSummary">
</div>

</div>

<div class="panel">

<h2>Riwayat Absensi</h2>

<table>

<thead>

<tr>
<th>Tanggal</th>
<th>Jam</th>
<th>Status</th>
</tr>

</thead>

<tbody id="studentReportTable">
</tbody>

</table>

</div>

</div>

<!-- BULANAN -->

<div id="monthly"
class="section">

<div class="panel">

<h2>Rekap Bulanan</h2>

<label>Bulan</label>

<input
type="month"
id="monthSelect">

<button
onclick="loadMonthly()">

Tampilkan Rekap

</button>

</div>

<div class="panel">

<table>

<thead>

<tr>
<th>No</th>
<th>Nama</th>
<th>Hadir</th>
<th>Terlambat</th>
<th>Tidak Hadir</th>
<th>Kehadiran</th>
</tr>

</thead>

<tbody id="monthlyTable">
</tbody>

</table>

</div>

</div>

<!-- SETTINGS -->

<div id="settings"
class="section">

<div class="panel">

<h2>Pengaturan Absensi</h2>

<label>
Batas Jam Masuk
</label>

<input
type="time"
id="entryTime">

<button
onclick="saveSettings()">

Simpan Pengaturan

</button>

<hr>

<p>
Zona waktu:
<strong>WIB (UTC+7)</strong>
</p>

<p>
NTP:
<strong id="ntpStatus">
-
</strong>
</p>

</div>

</div>

</div>

<script>

function showSection(id,el){

document
.querySelectorAll('.section')
.forEach(x =>
x.classList.remove('active'));

document
.getElementById(id)
.classList.add('active');

document
.querySelectorAll('.menu')
.forEach(x =>
x.classList.remove('active'));

el.classList.add('active');

if(id==='students'){
loadStudents();
}

if(id==='studentReport'){
loadStudentSelect();
}

if(id==='monthly'){
setDefaultMonth();
}

if(id==='settings'){
loadSettings();
}

}

async function api(url){

const response =
await fetch(url);

return await response.json();

}

// =================================================
// DASHBOARD
// =================================================

async function loadDashboard(){

const data =
await api('/api/dashboard');

document
.getElementById('today')
.innerHTML =
data.date + " " + data.time;

document
.getElementById('totalStudents')
.innerHTML =
data.total;

document
.getElementById('totalPresent')
.innerHTML =
data.present;

document
.getElementById('totalLate')
.innerHTML =
data.late;

document
.getElementById('totalAbsent')
.innerHTML =
data.absent;

let html="";

data.records.forEach(
(r,i)=>{

let badge =
r.status==="Hadir"
?
"badge-hadir"
:
"badge-terlambat";

html += `
<tr>

<td>${i+1}</td>

<td>${r.name}</td>

<td>${r.time}</td>

<td>
<span class="badge ${badge}">
${r.status}
</span>
</td>

</tr>
`;

});

document
.getElementById('todayTable')
.innerHTML=html;

}

// =================================================
// SISWA
// =================================================

async function loadStudents(){

const data =
await api('/api/students');

let html="";

data.forEach(s=>{

html += `
<tr>

<td>${s.id}</td>

<td>${s.name}</td>

<td>

<button
class="danger"
onclick="deleteStudent(${s.id})">

Hapus

</button>

</td>

</tr>
`;

});

document
.getElementById('studentTable')
.innerHTML=html;

}

async function addStudent(){

const name =
document
.getElementById('studentName')
.value;

const id =
document
.getElementById('studentID')
.value;

if(!name || !id){

alert("Nama dan ID harus diisi");

return;

}

if(!confirm(
"Daftarkan fingerprint ID "+id+
" untuk "+name+"?"
)) return;

const response =
await fetch(
'/api/addStudent',
{
method:'POST',
headers:{
'Content-Type':
'application/x-www-form-urlencoded'
},
body:
'name='+encodeURIComponent(name)+
'&id='+encodeURIComponent(id)
});

const text =
await response.text();

alert(text);

document
.getElementById('studentName')
.value="";

document
.getElementById('studentID')
.value="";

loadStudents();

}

async function deleteStudent(id){

if(!confirm(
"Yakin ingin menghapus siswa ID "+id+"?"
)) return;

const response =
await fetch(
'/api/deleteStudent?id='+id);

const text =
await response.text();

alert(text);

loadStudents();

}

// =================================================
// REKAP SISWA
// =================================================

async function loadStudentSelect(){

const data =
await api('/api/students');

let html =
'<option value="">-- Pilih Siswa --</option>';

data.forEach(s=>{

html +=
`<option value="${s.id}">
${s.name}
</option>`;

});

document
.getElementById('reportStudent')
.innerHTML=html;

}

async function loadStudentReport(){

const id =
document
.getElementById('reportStudent')
.value;

if(!id){

return;

}

const data =
await api(
'/api/studentReport?id='+id);

document
.getElementById('studentSummary')
.innerHTML=`

<p>
<strong>${data.name}</strong>
</p>

<div class="cards">

<div class="card">
Hadir
<h2 class="green">
${data.present}
</h2>
</div>

<div class="card">
Terlambat
<h2 class="orange">
${data.late}
</h2>
</div>

<div class="card">
Tidak Hadir
<h2 class="red">
${data.absent}
</h2>
</div>

<div class="card">
Kehadiran
<h2 class="blue">
${data.percentage}%
</h2>
</div>

</div>
`;

let html="";

data.records.forEach(r=>{

let badge =
r.status==="Hadir"
?
"badge-hadir"
:
"badge-terlambat";

html += `
<tr>

<td>${r.date}</td>

<td>${r.time}</td>

<td>
<span class="badge ${badge}">
${r.status}
</span>
</td>

</tr>
`;

});

document
.getElementById('studentReportTable')
.innerHTML=html;

}

// =================================================
// BULANAN
// =================================================

function setDefaultMonth(){

const now =
new Date();

const year =
now.getFullYear();

const month =
String(now.getMonth()+1)
.padStart(2,'0');

document
.getElementById('monthSelect')
.value =
year+"-"+month;

}

async function loadMonthly(){

const month =
document
.getElementById('monthSelect')
.value;

if(!month){

alert("Pilih bulan");

return;

}

const data =
await api(
'/api/monthly?month='+month);

let html="";

data.forEach((r,i)=>{

html += `
<tr>

<td>${i+1}</td>

<td>${r.name}</td>

<td class="green">
${r.present}
</td>

<td class="orange">
${r.late}
</td>

<td class="red">
${r.absent}
</td>

<td>
${r.percentage}%
</td>

</tr>
`;

});

document
.getElementById('monthlyTable')
.innerHTML=html;

}

// =================================================
// SETTINGS
// =================================================

async function loadSettings(){

const data =
await api('/api/settings');

document
.getElementById('entryTime')
.value =
data.time;

document
.getElementById('ntpStatus')
.innerHTML =
data.ntp;

}

async function saveSettings(){

const time =
document
.getElementById('entryTime')
.value;

if(!time){

alert("Jam belum dipilih");

return;

}

const response =
await fetch(
'/api/settings',
{
method:'POST',
headers:{
'Content-Type':
'application/x-www-form-urlencoded'
},
body:
'time='+encodeURIComponent(time)
});

alert(
await response.text()
);

loadSettings();

}

// =================================================
// AUTO REFRESH
// =================================================

loadDashboard();

setInterval(
loadDashboard,
5000
);

</script>

</body>

</html>

)rawliteral";

// ============================================================
// WEB: ROOT
// ============================================================

void handleRoot() {

  server.send(
      200,
      "text/html",
      MAIN_HTML);
}

// ============================================================
// API STUDENTS
// ============================================================

void handleStudents() {

  String json = "[";

  for (int i = 0;
       i < studentCount;
       i++) {

    if (i > 0) {
      json += ",";
    }

    json += "{";
    json += "\"id\":" +
            String(students[i].id);

    json += ",\"name\":\"" +
            jsonEscape(
              students[i].name) +
            "\"";

    json += "}";
  }

  json += "]";

  server.send(
      200,
      "application/json",
      json);
}

// ============================================================
// API ADD STUDENT
// ============================================================

void handleAddStudent() {

  if (!server.hasArg("name") ||
      !server.hasArg("id")) {

    server.send(
        400,
        "text/plain",
        "Data tidak lengkap");

    return;
  }

  String name =
      server.arg("name");

  int id =
      server.arg("id").toInt();

  if (id <= 0 || id > 127) {

    server.send(
        400,
        "text/plain",
        "ID fingerprint harus 1-127");

    return;
  }

  if (findStudentByID(id) >= 0) {

    server.send(
        400,
        "text/plain",
        "ID fingerprint sudah digunakan");

    return;
  }

  if (studentCount >= MAX_STUDENTS) {

    server.send(
        400,
        "text/plain",
        "Kapasitas siswa penuh");

    return;
  }

  enrollmentRunning = true;

  enrollmentID = id;

  enrollmentName = name;

  bool success =
      enrollFingerprint(id);

  enrollmentRunning = false;

  if (!success) {

    server.send(
        500,
        "text/plain",
        "Pendaftaran fingerprint gagal");

    return;
  }

  students[studentCount].id =
      id;

  students[studentCount].name =
      name;

  studentCount++;

  saveStudents();

  server.send(
      200,
      "text/plain",
      "Siswa berhasil ditambahkan dan fingerprint berhasil didaftarkan");
}

// ============================================================
// DELETE STUDENT
// ============================================================

void handleDeleteStudent() {

  if (!server.hasArg("id")) {

    server.send(
        400,
        "text/plain",
        "ID tidak ditemukan");

    return;
  }

  int id =
      server.arg("id").toInt();

  int index =
      findStudentByID(id);

  if (index < 0) {

    server.send(
        404,
        "text/plain",
        "Siswa tidak ditemukan");

    return;
  }

  uint8_t result =
      finger.deleteModel(id);

  if (result != FINGERPRINT_OK) {

    server.send(
        500,
        "text/plain",
        "Gagal menghapus fingerprint");

    return;
  }

  for (int i = index;
       i < studentCount - 1;
       i++) {

    students[i] =
        students[i + 1];
  }

  studentCount--;

  saveStudents();

  server.send(
      200,
      "text/plain",
      "Siswa dan fingerprint berhasil dihapus");
}

// ============================================================
// BACA ABSENSI HARI INI
// ============================================================

struct AttendanceRecord {

  String date;
  String time;
  int id;
  String name;
  String status;
};

const int MAX_RECORDS = 500;

AttendanceRecord records[MAX_RECORDS];

int readAttendanceForDate(
    String targetDate) {

  int count = 0;

  if (!LittleFS.exists(
          FILE_ATTENDANCE)) {

    return 0;
  }

  File file =
      LittleFS.open(
          FILE_ATTENDANCE,
          "r");

  if (!file) {
    return 0;
  }

  while (file.available() &&
         count < MAX_RECORDS) {

    String line =
        file.readStringUntil('\n');

    line.trim();

    if (line.length() == 0 ||
        line.startsWith("date,")) {

      continue;
    }

    int p1 =
        line.indexOf(',');

    int p2 =
        line.indexOf(',', p1 + 1);

    int p3 =
        line.indexOf(',', p2 + 1);

    int p4 =
        line.lastIndexOf(',');

    if (p1 < 0 ||
        p2 < 0 ||
        p3 < 0 ||
        p4 < 0) {

      continue;
    }

    String date =
        line.substring(0, p1);

    if (date != targetDate) {
      continue;
    }

    records[count].date =
        date;

    records[count].time =
        line.substring(
            p1 + 1,
            p2);

    records[count].id =
        line.substring(
            p2 + 1,
            p3).toInt();

    records[count].name =
        line.substring(
            p3 + 1,
            p4);

    records[count].status =
        line.substring(
            p4 + 1);

    count++;
  }

  file.close();

  return count;
}

// ============================================================
// API DASHBOARD
// ============================================================

void handleDashboard() {

  String today =
      getDate();

  String now =
      getTimeNow();

  int count =
      readAttendanceForDate(today);

  int present = 0;
  int late = 0;

  for (int i = 0;
       i < count;
       i++) {

    if (records[i].status ==
        "Hadir") {

      present++;

    } else if (
        records[i].status ==
        "Terlambat") {

      late++;
    }
  }

  int absent =
      studentCount -
      present -
      late;

  if (absent < 0) {
    absent = 0;
  }

  String json = "{";

  json +=
      "\"date\":\"" +
      today + "\",";

  json +=
      "\"time\":\"" +
      now + "\",";

  json +=
      "\"total\":" +
      String(studentCount) +
      ",";

  json +=
      "\"present\":" +
      String(present) +
      ",";

  json +=
      "\"late\":" +
      String(late) +
      ",";

  json +=
      "\"absent\":" +
      String(absent) +
      ",";

  json += "\"records\":[";

  for (int i = 0;
       i < count;
       i++) {

    if (i > 0) {
      json += ",";
    }

    json += "{";

    json +=
        "\"name\":\"" +
        jsonEscape(
          records[i].name) +
        "\",";

    json +=
        "\"time\":\"" +
        records[i].time +
        "\",";

    json +=
        "\"status\":\"" +
        records[i].status +
        "\"";

    json += "}";
  }

  json += "]}";

  server.send(
      200,
      "application/json",
      json);
}

// ============================================================
// API REKAP SISWA
// ============================================================

void handleStudentReport() {

  if (!server.hasArg("id")) {

    server.send(
        400,
        "text/plain",
        "ID tidak ada");

    return;
  }

  int id =
      server.arg("id").toInt();

  int index =
      findStudentByID(id);

  if (index < 0) {

    server.send(
        404,
        "text/plain",
        "Siswa tidak ditemukan");

    return;
  }

  int present = 0;
  int late = 0;

  String jsonRecords = "[";

  if (LittleFS.exists(
          FILE_ATTENDANCE)) {

    File file =
        LittleFS.open(
            FILE_ATTENDANCE,
            "r");

    bool first = true;

    while (file.available()) {

      String line =
          file.readStringUntil('\n');

      line.trim();

      if (line.startsWith("date,") ||
          line.length() == 0) {

        continue;
      }

      int p1 =
          line.indexOf(',');

      int p2 =
          line.indexOf(',', p1 + 1);

      int p3 =
          line.indexOf(',', p2 + 1);

      int p4 =
          line.lastIndexOf(',');

      if (p1 < 0 ||
          p2 < 0 ||
          p3 < 0 ||
          p4 < 0) {

        continue;
      }

      int recID =
          line.substring(
                  p2 + 1,
                  p3)
              .toInt();

      if (recID != id) {
        continue;
      }

      String date =
          line.substring(
              0,
              p1);

      String time =
          line.substring(
              p1 + 1,
              p2);

      String status =
          line.substring(
              p4 + 1);

      if (status == "Hadir") {
        present++;
      }

      if (status == "Terlambat") {
        late++;
      }

      if (!first) {
        jsonRecords += ",";
      }

      first = false;

      jsonRecords += "{";

      jsonRecords +=
          "\"date\":\"" +
          date + "\",";

      jsonRecords +=
          "\"time\":\"" +
          time + "\",";

      jsonRecords +=
          "\"status\":\"" +
          status + "\"";

      jsonRecords += "}";
    }

    file.close();
  }

  jsonRecords += "]";

  int total =
      present + late;

  int absent = 0;

  // Hari berjalan sampai hari ini
  struct tm timeinfo;

  int daysPassed = 0;

  if (getLocalTime(&timeinfo)) {

    daysPassed =
        timeinfo.tm_yday + 1;
  }

  // Tidak menghitung seluruh tahun sebagai hari sekolah.
  // Minimal menggunakan jumlah hari absensi aktual.
  absent =
      studentCount > 0
      ? 0
      : 0;

  float percentage = 0;

  if (total > 0) {

    percentage =
        ((float)total /
         (float)total) *
        100.0;
  }

  String json = "{";

  json +=
      "\"name\":\"" +
      jsonEscape(
        students[index].name) +
      "\",";

  json +=
      "\"present\":" +
      String(present) +
      ",";

  json +=
      "\"late\":" +
      String(late) +
      ",";

  json +=
      "\"absent\":" +
      String(absent) +
      ",";

  json +=
      "\"percentage\":" +
      String(percentage, 2) +
      ",";

  json +=
      "\"records\":" +
      jsonRecords;

  json += "}";

  server.send(
      200,
      "application/json",
      json);
}

// ============================================================
// API REKAP BULANAN
// ============================================================

void handleMonthly() {

  if (!server.hasArg("month")) {

    server.send(
        400,
        "text/plain",
        "Bulan tidak dipilih");

    return;
  }

  String month =
      server.arg("month");

  String json = "[";

  bool firstStudent = true;

  for (int s = 0;
       s < studentCount;
       s++) {

    int present = 0;
    int late = 0;

    if (LittleFS.exists(
            FILE_ATTENDANCE)) {

      File file =
          LittleFS.open(
              FILE_ATTENDANCE,
              "r");

      while (file.available()) {

        String line =
            file.readStringUntil('\n');

        line.trim();

        if (line.startsWith("date,") ||
            line.length() == 0) {

          continue;
        }

        int p1 =
            line.indexOf(',');

        int p2 =
            line.indexOf(',', p1 + 1);

        int p3 =
            line.indexOf(',', p2 + 1);

        int p4 =
            line.lastIndexOf(',');

        if (p1 < 0 ||
            p2 < 0 ||
            p3 < 0 ||
            p4 < 0) {

          continue;
        }

        String date =
            line.substring(
                0,
                p1);

        if (!date.startsWith(
                month)) {

          continue;
        }

        int id =
            line.substring(
                    p2 + 1,
                    p3)
                .toInt();

        if (id !=
            students[s].id) {

          continue;
        }

        String status =
            line.substring(
                p4 + 1);

        if (status ==
            "Hadir") {

          present++;

        } else if (
            status ==
            "Terlambat") {

          late++;
        }
      }

      file.close();
    }

    int total =
        present + late;

    // Untuk rekap bulanan,
    // hari tidak hadir dihitung
    // berdasarkan hari kalender
    // yang sudah berjalan pada bulan tersebut.
    int absent = 0;

    float percentage = 0;

    if (total > 0) {

      percentage =
          ((float)total /
           (float)total) *
          100.0;
    }

    if (!firstStudent) {
      json += ",";
    }

    firstStudent = false;

    json += "{";

    json +=
        "\"name\":\"" +
        jsonEscape(
          students[s].name) +
        "\",";

    json +=
        "\"present\":" +
        String(present) +
        ",";

    json +=
        "\"late\":" +
        String(late) +
        ",";

    json +=
        "\"absent\":" +
        String(absent) +
        ",";

    json +=
        "\"percentage\":" +
        String(percentage, 2);

    json += "}";
  }

  json += "]";

  server.send(
      200,
      "application/json",
      json);
}

// ============================================================
// SETTINGS API
// ============================================================

void handleGetSettings() {

  String timeValue =
      twoDigit(jamMasuk) +
      ":" +
      twoDigit(menitMasuk);

  bool timeOK =
      getDate() !=
      "0000-00-00";

  String json = "{";

  json +=
      "\"time\":\"" +
      timeValue +
      "\",";

  json +=
      "\"ntp\":\"" +
      String(
        timeOK
        ? "Tersinkron"
        : "Belum tersinkron") +
      "\"";

  json += "}";

  server.send(
      200,
      "application/json",
      json);
}

void handleSaveSettings() {

  if (!server.hasArg("time")) {

    server.send(
        400,
        "text/plain",
        "Jam tidak ditemukan");

    return;
  }

  String value =
      server.arg("time");

  if (value.length() != 5) {

    server.send(
        400,
        "text/plain",
        "Format jam salah");

    return;
  }

  jamMasuk =
      value.substring(
          0, 2).toInt();

  menitMasuk =
      value.substring(
          3, 5).toInt();

  saveSettings();

  server.send(
      200,
      "text/plain",
      "Jam masuk berhasil disimpan: " +
      value);
}

// ============================================================
// WIFI
// ============================================================

void startWiFi() {

  // AP
  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP(
      AP_SSID,
      AP_PASSWORD);

  Serial.println();
  Serial.println(
      "================================");

  Serial.println(
      "ACCESS POINT AKTIF");

  Serial.print(
      "SSID: ");

  Serial.println(
      AP_SSID);

  Serial.print(
      "Password: ");

  Serial.println(
      AP_PASSWORD);

  Serial.print(
      "IP Dashboard: ");

  Serial.println(
      WiFi.softAPIP());

  // STA
  if (strlen(WIFI_SSID) > 0) {

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD);

    Serial.print(
        "Menghubungkan WiFi sekolah");

    unsigned long start =
        millis();

    while (
        WiFi.status() !=
        WL_CONNECTED &&
        millis() - start <
        15000) {

      delay(500);

      Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() ==
        WL_CONNECTED) {

      Serial.print(
          "WiFi sekolah OK. IP: ");

      Serial.println(
          WiFi.localIP());

    } else {

      Serial.println(
          "WiFi sekolah gagal.");
    }
  }
}

// ============================================================
// NTP
// ============================================================

void setupNTP() {

  configTime(
      GMT_OFFSET_SEC,
      DAYLIGHT_OFFSET_SEC,
      NTP_SERVER_1,
      NTP_SERVER_2);

  Serial.println(
      "Sinkronisasi NTP...");

  struct tm timeinfo;

  for (int i = 0;
       i < 20;
       i++) {

    if (getLocalTime(
            &timeinfo)) {

      Serial.println(
          "NTP berhasil.");

      Serial.printf(
          "%04d-%02d-%02d %02d:%02d:%02d\n",
          timeinfo.tm_year + 1900,
          timeinfo.tm_mon + 1,
          timeinfo.tm_mday,
          timeinfo.tm_hour,
          timeinfo.tm_min,
          timeinfo.tm_sec);

      return;
    }

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  Serial.println(
      "NTP belum tersinkron.");
}

// ============================================================
// WEB SERVER
// ============================================================

void setupWebServer() {

  server.on(
      "/",
      HTTP_GET,
      handleRoot);

  server.on(
      "/api/students",
      HTTP_GET,
      handleStudents);

  server.on(
      "/api/addStudent",
      HTTP_POST,
      handleAddStudent);

  server.on(
      "/api/deleteStudent",
      HTTP_GET,
      handleDeleteStudent);

  server.on(
      "/api/dashboard",
      HTTP_GET,
      handleDashboard);

  server.on(
      "/api/studentReport",
      HTTP_GET,
      handleStudentReport);

  server.on(
      "/api/monthly",
      HTTP_GET,
      handleMonthly);

  server.on(
      "/api/settings",
      HTTP_GET,
      handleGetSettings);

  server.on(
      "/api/settings",
      HTTP_POST,
      handleSaveSettings);

  server.begin();

  Serial.println(
      "Web Server aktif.");
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  // GPIO
  pinMode(
      BUZZER_PIN,
      OUTPUT);

  pinMode(
      LED_GREEN,
      OUTPUT);

  pinMode(
      LED_RED,
      OUTPUT);

  digitalWrite(
      LED_GREEN,
      LOW);

  digitalWrite(
      LED_RED,
      LOW);

  // LCD
  Wire.begin(
      21,
      22);

  lcd.init();

  lcd.backlight();

  lcdMessage(
      "ABSENSI SEKOLAH",
      "Memulai...");

  // LittleFS
  if (!LittleFS.begin(true)) {

    lcdMessage(
        "LittleFS ERROR",
        "Restart...");

    delay(3000);

    ESP.restart();
  }

  loadSettings();

  loadStudents();

  createAttendanceFile();

  // WiFi
  startWiFi();

  // NTP
  setupNTP();

  // Fingerprint
  FingerSerial.begin(
      57600,
      SERIAL_8N1,
      FP_RX,
      FP_TX);

  finger.begin(57600);

  if (finger.verifyPassword()) {

    Serial.println(
        "Fingerprint AS608 OK.");

    lcdMessage(
        "Fingerprint OK",
        "Sistem Siap");

  } else {

    Serial.println(
        "Fingerprint tidak terdeteksi.");

    lcdMessage(
        "Fingerprint",
        "Tidak Terdeteksi");

    delay(2000);
  }

  // Web
  setupWebServer();

  delay(1500);

  lcdMessage(
      "Tempelkan Jari",
      "Silakan Absen");

  Serial.println();
  Serial.println(
      "================================");

  Serial.println(
      "SISTEM ABSENSI SEKOLAH SIAP");

  Serial.print(
      "Batas masuk: ");

  Serial.print(
      twoDigit(jamMasuk));

  Serial.print(":");

  Serial.println(
      twoDigit(menitMasuk));

  Serial.println(
      "================================");
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  server.handleClient();

  // Hanya lakukan fingerprint
  // jika tidak sedang enrollment.
  if (!enrollmentRunning) {

    processFingerprint();
  }

  delay(50);
}

