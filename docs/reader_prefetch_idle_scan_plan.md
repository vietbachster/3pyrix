# Kế hoạch triển khai: EPUB chapter prefetch + idle spine page-count scan

## Mục tiêu
- Thêm prefetch chapter EPUB lân cận để giảm độ trễ khi đổi chapter.
- Thêm idle spine page-count scan theo kiểu `best-effort` để tăng độ chính xác và độ tức thời cho thông tin tiến độ.
- Không làm tăng cảm giác lag UI; mọi việc nền phải dừng nhanh khi có tương tác.

## Nguyên tắc thiết kế
1. **Không chạm fast path của chapter hiện tại**:
   - Chapter đang đọc vẫn ưu tiên tuyệt đối cho render/lật trang.
   - Prefetch/scan chỉ chạy khi cache chapter hiện tại đã sẵn sàng.
2. **Cooperative cancellation**:
   - Tách nhỏ công việc theo chapter/spine item.
   - Kiểm tra tín hiệu dừng giữa các bước I/O nặng.
3. **Best-effort + incremental persistence**:
   - Scan page-count lưu dần từng kết quả, không đợi hoàn tất toàn bộ spine.
   - Nếu sleep/thoát giữa chừng thì vẫn giữ được phần đã scan.
4. **Không block UI khi stop background**:
   - Tránh các đoạn I/O đơn khối quá dài không thể hủy.

## Phạm vi file đề xuất chỉnh sửa
- `src/states/ReaderState.h`
- `src/states/ReaderState.cpp`
- `src/content/BookProgress.cpp` (hoặc lớp lưu progress hiện có liên quan page-count)
- `src/content/BookProgress.h`

> Ghi chú: tên class/file progress có thể thay đổi nhẹ theo codebase; khi implement cần map sang lớp đang lưu metadata tiến độ thực tế.

## API/state cần bổ sung trong ReaderState
### 1) Cờ trạng thái background pipeline
- `bool prefetchEnabled_`
- `bool pageCountScanEnabled_`
- `uint16_t prefetchedSpineIndex_` (hoặc sentinel khi chưa có)
- `uint16_t scanCursorSpineIndex_`
- `bool scanCompleted_`

### 2) Cấu hình guard để tránh tranh tài nguyên
- Chỉ chạy prefetch/scan khi:
  - Không có input gần đây (reuse điều kiện idle hiện có).
  - Không có thao tác UI blocking (menu/chapter jump/open toc).
  - Không có task nền khác đang chiếm parser/page cache.

### 3) Queue ưu tiên công việc nền
Ưu tiên gợi ý:
1. Build cache chapter hiện tại (đang có).
2. Prefetch chapter kề bên (next trước, rồi prev nếu cần).
3. Scan page-count spine theo cursor.

## Luồng thực thi đề xuất
### A. Sau khi chapter hiện tại cache xong
1. Xác định `nextSpine` hợp lệ.
2. Nếu chưa prefetched và bộ nhớ cho phép:
   - Parse tối thiểu + tính page count + lưu header cache cần thiết cho `nextSpine`.
   - Không giữ object nặng lâu hơn cần thiết.
3. Đánh dấu `prefetchedSpineIndex_ = nextSpine`.

### B. Idle page-count scan
1. Khởi tạo `scanCursorSpineIndex_` từ chapter hiện tại + 1 (vòng tròn qua đầu spine).
2. Mỗi “tick nền” xử lý tối đa **1 spine item**.
3. Sau mỗi item:
   - Ghi kết quả page-count vào storage tiến độ.
   - Kiểm tra stop signal trước khi sang item tiếp theo.
4. Khi scan đủ toàn spine thì set `scanCompleted_ = true`.

## Cooperative cancellation chi tiết
Trong các vòng lặp parse/estimate pages:
- Trước khi mở file/chapter mới: kiểm tra `shouldStop()`.
- Sau mỗi block đọc/parse lớn: kiểm tra `shouldStop()`.
- Trước khi ghi kết quả: kiểm tra `shouldStop()` để tránh flush thừa khi user vừa thao tác.

Nếu nhận stop:
- Commit phần dữ liệu đã có thể commit an toàn.
- Thoát ngay, không làm bước tối ưu hóa hậu kỳ.

## Persistence cho page-count
- Mở rộng model tiến độ để chứa map `spineIndex -> pageCount` và timestamp cập nhật.
- Cơ chế ghi:
  - Debounce nhẹ (ví dụ sau mỗi N item hoặc sau X giây).
  - Nhưng vẫn flush khi rời ReaderState/sleep để hạn chế mất dữ liệu scan.
- Khi mở sách:
  - Dùng page-count đã scan nếu còn hợp lệ.
  - Spine chưa scan thì hiển thị trạng thái “đang tính”.

## Tích hợp với throttle/sleep hiện có
- CPU throttle idle không xung đột chức năng; chỉ làm scan/prefetch chậm hơn.
- Vì vậy pipeline phải được coi là **background opportunistic**:
  - Không kỳ vọng hoàn thành trong một phiên idle.
  - Chấp nhận bị sleep cắt ngang và resume ở lần sau qua `scanCursorSpineIndex_`.

## Đo lường sau triển khai
### Chỉ số cần log
- `t_open_to_first_page`
- `t_turn_chapter_boundary`
- `t_toc_open_after_idle`
- `stop_background_latency_ms`
- `% spine scanned after 1/3/5 phút idle`

### Tiêu chí đạt
- Không tăng `stop_background_latency_ms` quá ngưỡng UX (ví dụ >150ms ở median).
- Giảm rõ rệt thời gian qua ranh giới chapter.
- Không tăng lỗi sleep/wake hoặc crash parser.

## Rollout an toàn
1. Thêm compile-time flag (ví dụ `ENABLE_EPUB_BACKGROUND_PREFETCH_SCAN`).
2. Bật mặc định trong nhánh dev, tắt mặc định cho release đầu.
3. Thu log benchmark + pin tiêu thụ.
4. Khi ổn định mới bật mặc định cho release chính thức.

## Kế hoạch triển khai theo commit nhỏ
1. **Commit 1**: thêm state + scheduling khung trong `ReaderState` (chưa parse thật).
2. **Commit 2**: implement chapter-adjacent prefetch (next chapter).
3. **Commit 3**: implement idle spine page-count scan + persistence incremental.
4. **Commit 4**: thêm guard/cancellation và log đo latency.
5. **Commit 5**: tinh chỉnh memory/CPU + cleanup.

## Rủi ro chính và cách chặn
- **Rủi ro**: Parser dùng chung tài nguyên với chapter hiện tại gây contention.
  - **Chặn**: chỉ cho nền chạy sau khi tác vụ foreground ổn định; không reuse object mutable của foreground.
- **Rủi ro**: stop chậm do I/O dài.
  - **Chặn**: chia nhỏ chunk đọc/parse, check stop giữa chunk.
- **Rủi ro**: ghi persistence quá dày làm tăng hao pin.
  - **Chặn**: debounce + flush ở điểm lifecycle quan trọng.

## Gợi ý tiếp theo (ngoài phạm vi bản này)
- Prefetch hai chiều (next + previous) khi user có xu hướng đọc lùi/chú thích.
- Heuristic theo hành vi đọc để quyết định độ sâu prefetch.
- Tích hợp hiển thị tiến trình scan nhỏ trong UI debug/perf overlay.
