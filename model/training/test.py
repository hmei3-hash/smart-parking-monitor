# test_stream.py
# 先测试能不能看到 ESP32-CAM 的画面

import cv2

stream_url ="http://192.168.137.133:81/stream"  # 改成你的 IP

cap = cv2.VideoCapture(stream_url)
if not cap.isOpened():
    print("Cannot open stream")
    exit()

print("Stream opened. Press 'q' to quit.")

while True:
    ret, frame = cap.read()
    if not ret:
        print("Frame lost, retrying...")
        continue

    # 显示分辨率信息
    h, w = frame.shape[:2]
    cv2.putText(frame, f"{w}x{h}", (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
    cv2.imshow("ESP32-CAM Test", frame)

    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

cap.release()
cv2.destroyAllWindows()