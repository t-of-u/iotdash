# iotdash

SDK สำหรับบอร์ดจอสัมผัส ESP32-S3 7" (Waveshare ESP32-S3-Touch-LCD-7, 800×480): engine ที่วาด
HTML/CSS/JS บนชิปเอง, OS (home, Settings, Apps store, Wi-Fi, Bluetooth), simulator บน desktop และใน
เบราว์เซอร์ และเครื่องมือ command line `iotdash` แบบเดียวกับ Flutter: แอปเป็นโปรเจกต์ของคุณ อยู่ที่ไหนก็ได้

```sh
scripts/setup.sh                              # ครั้งแรก: Python tools + Chromium, ภาพระบบ, simulator
export PATH="$PATH:$PWD/bin"                  # ใส่ใน ~/.zshrc
iotdash doctor

iotdash create ~/projects/hello && cd ~/projects/hello
iotdash run                                   # simulator
iotdash run --device                          # บอร์ดผ่าน USB
iotdash run --web                             # เบราว์เซอร์ (scripts/setup.sh --web)
iotdash pack                                  # build/hello-1.0.idbz สำหรับ store
iotdash publish --store https://…             # ขึ้น iotstore
```

ครั้งแรกกับบอร์ด: `iotdash os build && iotdash firmware upload && iotdash os deploy --to device`

| โฟลเดอร์ | |
|---|---|
| `bin/iotdash` | command line (ใส่ `bin/` ใน PATH) |
| `scripts/setup.sh` | ติดตั้ง SDK ครั้งแรก (`--web`, `--firmware`) |
| `templates/app/` | template ของ `iotdash create` |
| `examples/` | แอปตัวอย่าง: `iotdash run calculator` |
| `lib/rt/` | engine และ OS kernel (C++17, portable) |
| `src/` | firmware ของบอร์ด (PlatformIO, `iotdash firmware`) |
| `os/core/` | ซอร์สของ OS: `os.js` (JS API), home, Settings, Apps, shell, theme |
| `sim/`, `web/` | simulator บน desktop และใน WebAssembly |
| `store/` | app store ที่ host เองได้ (React + Vite, Lambda + DynamoDB + S3): `iotdash store dev / test / deploy` |
| `tools/` | compiler, CLI (`iotdash.py`, `project.py`, `osbuild.py`), รูปแบบแพ็กเกจ (`idbz.py`), store client |
| `docs/` | เอกสารทั้งหมด (เปิด `docs/index.html`) |

แอปที่เขียนด้วย SDK เป็นของผู้เขียน: SDK ไม่ต้องมีโค้ดของแอปอยู่ข้างใน และ store
([`store/`](store), `iotdash store dev|deploy`) เป็นเพียงที่เก็บแพ็กเกจที่ใครก็ host เองได้

เอกสาร: [ติดตั้งและใช้งานครั้งแรก](docs/guide/getting-started.html) ·
[เขียนแอป](docs/guide/app-development.html) · [command line](docs/reference/cli.html) ·
[JavaScript API](docs/reference/js-api.html) · [ข้อจำกัดที่รู้](docs/project/known-issues.html)

## License

[MIT](LICENSE) ทั้ง SDK และ store ส่วนที่มาจากโปรเจกต์อื่นใช้ license ของตัวเอง: QuickJS-ng
([MIT](lib/quickjs/LICENSE)) และฟอนต์ Inter, Noto Sans Thai, JetBrains Mono
([SIL OFL 1.1](tools/fonts)) แอปที่สร้างด้วย SDK เป็นของผู้เขียน จะใช้ license อะไรก็ได้
