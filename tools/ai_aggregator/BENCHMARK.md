# BENCHMARK — AI Aggregator trên WSL

Đo thật trên máy build, không ước lượng. Ngày chạy: 2026-10-01.

## Máy

| | |
|---|---|
| CPU | Intel Core i5-8350U @ 1.70GHz, 8 thread |
| `torch.get_num_threads()` | **4** (không phải 8) |
| GPU | không có |
| torch | 2.14.1+cpu / torchvision 0.29.1+cpu |
| RAM | 5.9 GB tmpfs cho `/tmp`, phải đặt venv ngoài tmpfs |
| Input test | `bus.jpg` 810x1080 → video 30fps 1280x720 |

## Baseline: decode không có AI

| | fps |
|---|---|
| `FrameReader` (ffmpeg rawvideo) | 165 |
| `cv2.VideoCapture` | 606 |

Decode **không phải** nút thắt. Toàn bộ chi phí nằm ở inference.

## Chi phí từng detector (đơn lẻ, imgsz=640, mỗi frame)

| detector | load s | mean ms | p50 | p95 | max fps |
|---|---|---|---|---|---|
| presence | 0.40 | 92.9 | 81.2 | 166.1 | 10.8 |
| vehicle | 0.18 | 99.3 | 83.5 | 154.2 | 10.1 |
| fall | 0.20 | 112.9 | 102.1 | 155.5 | 8.9 |
| intrusion | 0.18 | 105.0 | 96.3 | 154.7 | 9.5 |
| person_count | 0.26 | 120.4 | 91.3 | 213.9 | 8.3 |
| crowd | 0.19 | 122.9 | 117.5 | 166.3 | 8.1 |

Tổng 6 detector: **653 ms/frame** → trần 1.5 fps nếu bật hết.

## imgsz là đòn bẩy lớn nhất

presence, `every_n_frames=1`:

| imgsz | fps | mean ms |
|---|---|---|
| 640 | 8.6 | 111.2 |
| 480 | 11.8 | 78.5 |
| 320 | 18.0 | 49.9 |

`OMP_NUM_THREADS=8` **không giúp** — torch tự giới hạn 4 thread và ép 8 còn chậm hơn (fall: 93.9 → 120.1 ms). Đừng tăng biến này.

## Tổ hợp thực tế (detect + draw)

| cấu hình | fps | mean | p95 | % frame > 33ms |
|---|---|---|---|---|
| presence(320, e2) | 32.8 | 26.1 | 72.6 | 43.7% |
| fall(320, e2) | 23.8 | 38.0 | 109.9 | 50.0% |
| presence(320,e2) + fall(320,e3) | 15.2 | 62.0 | 184.0 | 64.1% |
| presence+fall+crowd+intrusion | 12.0 | 79.2 | 302.4 | 64.9% |

## Kết luận quan trọng

**Máy này KHÔNG đủ sức chạy nhiều detector ở 30 fps.** Ngay cả một detector đơn lẻ ở `imgsz=640` cũng chỉ ~10 fps.

Khuyến nghị thực tế cho CPU-only i5-8350U:

1. **`imgsz: 320`** — bắt buộc. Đây là thay đổi lớn nhất.
2. **Một detector nặng + một detector nhẹ.** Ví dụ `presence(e2) + fall(e3)` chạy ~15 fps.
3. **`every_n_frames: 2-6`** — đây là chỗ cần giữ khung hình vẽ ở mọi frame để overlay không nhấp nháy, nên `every_n_frames` không ảnh hưởng độ mượt hình, chỉ ảnh hưởng độ trễ phát hiện.
4. **Chấp nhận 12-15 fps** cho output nếu bật 3-4 detector. YouTube vẫn nhận; chỉ overlay chậm hơn thực tế.

## Vì sao vẫn đạt "30fps" về mặt hình ảnh

`FrameWriter` không chặn detection loop; nếu encode chậm, frame bị drop. Nhưng nếu cả detect lẫn encode đều vượt 33.3 ms thì output thực sự chậm — đo được ở bảng trên. Overlay vẫn mượt về mặt thị giác vì kết quả được giữ giữa các lần suy luận, chỉ chậm đi.

## Cảnh báo cài đặt

- `torch` phải cài từ `--index-url https://download.pytorch.org/whl/cpu`. Bản mặc định kéo ~1.6 GB CUDA wheels.
- `torchvision` **cũng phải** cài từ index CPU. Cài torchvision từ PyPI (bản CUDA) với torch CPU sẽ hỏng lúc chạy:
  `RuntimeError: operator torchvision::nms does not exist`.
- `/tmp` trên WSL là tmpfs 5.9 GB — không đủ cho venv torch (~1.5 GB + deps). Đặt venv ngoài tmpfs, ví dụ `~/.miicam-aivenv`.