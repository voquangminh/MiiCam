# AI Aggregator (WSL)

Gộp nhiều detector AI vào **một** luồng RTSP rồi đẩy lên YouTube.

## Vì sao phải có

| Sự thật | Hệ quả |
|---|---|
| Camera GM8136 chạy ARMv5TE | Không có bản PyTorch cho SoC này → YOLO **không thể** chạy trên camera. Inference buộc phải ở máy khác. |
| Camera chỉ nhận **một** phiên RTSP | Mọi detector phải dùng chung 1 kết nối. Chạy script riêng lẻ sẽ mở N phiên → treo camera. |
| YouTube RTMP chỉ nhận **pixel đã encode** | Không có khái niệm bounding box / "node". Overlay **bắt buộc** phải được vẽ vào frame trước khi encode. |

Repo tham khảo `Nawaf-Rayhan585/YOLO_Projects` gồm 17 script độc lập, mỗi script tự mở `cv2.VideoCapture` và tự `imshow`. Cần viết lại thành một aggregator dùng chung — đó là nội dung thư mục này.

## Luồng

```
camera rtspd ──RTSP (1 kết nối)──> [aggregator.py]
  192.168.1.204                      ├─ gọi N detector trên cùng frame
                                     ├─ vẽ boxes / keypoints / HUD panel
                                     └─ pipe rawvideo ──> ffmpeg ──RTMP──> YouTube
```

RTMP push chạy trên Note9 (`rtsp2rtmp`) nên mặc định `output.rtmp` để trống; xem [Tích hợp Note9](#tích-hợp-note9).

## Cài đặt

```bash
sudo apt install -y python3-venv ffmpeg
python3 -m venv .venv
.venv/bin/pip install --no-cache-dir \
    torch --index-url https://download.pytorch.org/whl/cpu
.venv/bin/pip install --no-cache-dir \
    "ultralytics>=8.3.0" opencv-python-headless numpy supervision pyyaml
```

Bắt buộc `--index-url .../cpu`: bản torch mặc định kéo ~1.6GB CUDA wheels vô ích trên máy không có GPU.

## Chạy

Nguồn vào lấy từ `source.rtsp` trong `config.yaml`, giá trị mặc định là
`${AI_RTSP_URL}`. Cần set biến môi trường vì config không chứa mật khẩu:

```bash
export AI_RTSP_URL='rtsp://user:pass@192.168.1.204:554/live/ch00_0'
```

Đè nguồn khác bằng `--source` (ffmpeg đọc được là được).

```bash
# xem detector có sẵn
.venv/bin/python aggregator.py --list

# bật detector, xem thử ra file (không push)
.venv/bin/python aggregator.py -e presence -e fall --preview /tmp/out.mp4

# chạy theo config.yaml
.venv/bin/python aggregator.py -c config.yaml
```

Xem detector nào đang bật và trạng thái weights:

```bash
.venv/bin/python aggregator.py -c config.yaml -e fire --status
```

## Bật/tắt từng detector

Sửa `config.yaml`, mỗi mục có `enabled: false` mặc định:

| Detector | Model mặc định | Cần weights riêng? | Ghi chú |
|---|---|---|---|
| `presence` | `yolo11n.pt` | không | Rẻ nhất, `every_n_frames: 3` |
| `crowd` | `yolo11n.pt` | không | Heatmap phai dần + đếm người |
| `fall` | `yolo11n-pose.pt` | không | Pose heuristic, không cần train |
| `intrusion` | `yolo11n.pt` | không | Vùng `polygon` chuẩn hoá 0..1 |
| `person_count` | `yolo11n.pt` | không | Đếm qua đường dọc `line` |
| `vehicle` | `yolo11n.pt` | không | COCO car/bus/truck/moto |
| `fire` | `fire.pt` | **có** | Roboflow `best.pt` → `weights/fire.pt` |
| `ppe` | `ppe.pt` | **có** | Roboflow `best.pt` |
| `rat` | `rat.pt` | **có** | Roboflow `best.pt` |
| `plate` | `plate.pt` | **có** | Roboflow `best.pt` |

Detector thiếu weights **không làm sập luồng** — aggregator báo lỗi rồi tự gỡ detector đó khỏi vòng chạy, các detector còn lại tiếp tục. Nên bật sẵn `fire`/`ppe`/`rat` cũng an toàn trước khi bỏ file vào.

Đặt weights vào `weights/` (hoặc `AI_WEIGHTS_DIR`), tên file theo `model:` trong config.

## Hiệu năng

Máy build này: i5-8350U, 8 thread, **CPU-only**. Số đo thực tế ghi trong `BENCHMARK.md`.

Nguyên tắc giữ 30fps khi detect chậm hơn 30fps:

- `every_n_frames` — detector chỉ suy luận 1/N frame, aggregator giữ kết quả giữa các lần.
- Overlay vẽ ở mọi frame nên hình **không nhấp nháy**.
- `FrameWriter` không chặn detection loop; nếu ffmpeg chậm thì frame bị drop.

Bật tất cả cùng lúc sẽ vượt CPU i5-8350U. Bật dần, đo `detect_ms` trên HUD.

## Tích hợp Note9

RTMP đang chạy trên Note9 qua `rtsp2rtmp`. Cho aggregator đẩy thẳng:

```bash
.venv/bin/python aggregator.py -e fall -e fire \
  --rtmp "rtmp://a.rtmp.youtube.com/live2/<STREAM_KEY>"
```

Cho Note9 pull bản đã annotate (giữ luồng RTMP sẵn có) — **chưa hỗ trợ, cần viết thêm**:

1. Thêm RTSP server vào `aggregator.py` (tái tụ `tools/rtsp_server/live_http.c` bằng Python
   hoặc bọc ffmpeg `-f rtsp -rtsp_transport tcp`). Hiện aggregator chỉ xuất RTMP
   hoặc file preview qua `--preview`.
2. Trên Note9 đổi `cameras.conf` sang `rtsp://100.117.21.50:8554/annotated`.

Nhớ IP Tailscale của WSL là `100.117.21.50` (hostname `miicam-host`).

Trong lúc chờ, cách nhanh nhất để xem overlay là `--preview out.mp4`.

## Hướng Home Assistant (S905X3)

X96 Air + S905X3 là aarch64. HA chạy được, nhưng YOLO trên S905X3 rất chậm — nên chạy aggregator ở WSL và đẩy kết quả về HA qua MQTT:

- Bật `hud` panel để xác nhận overlay đúng, rồi xuất detection ra JSON — **chưa có**,
  `--json-out` chưa được implement. Cần thêm một writer ghi từng frame ra JSONL hoặc
  publish MQTT (`homeassistant/.../presence`, `.../person_count`).
- HA subscribe topic, tạo `binary_sensor` / `sensor` theo từng node.

S905X3 nên để làm HA host, không làm inference host.