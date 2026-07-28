# collect_data.py
# 从 ESP32-CAM stream 采集训练数据
# 按空格保存当前帧，按 o/e 切换标签，按 q 退出

import cv2
import os
import time

# ================== 配置 ====================
STREAM_URL = "http://192.168.137.133:81/stream"  # 改成你 CAM 的 IP

# ================== 初始化 ====================
os.makedirs("dataset/occupied", exist_ok=True)
os.makedirs("dataset/empty", exist_ok=True)

cap = cv2.VideoCapture(STREAM_URL)
if not cap.isOpened():
    print("Cannot open stream")
    exit()

label = "empty"
count = 0

print("Controls:")
print("  SPACE = save frame")
print("  o     = switch to 'occupied'")
print("  e     = switch to 'empty'")
print("  q     = quit")
print(f"\nCurrent label: {label}")

while True:
    ret, frame = cap.read()
    if not ret:
        continue

    cv2.putText(frame, f"{label} | saved: {count}", (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
    cv2.imshow("Collector", frame)

    key = cv2.waitKey(1) & 0xFF
    if key == ord(' '):
        path = f"dataset/{label}/{label}_{int(time.time())}_{count}.jpg"
        cv2.imwrite(path, frame)
        count += 1
        print(f"Saved: {path}")
    elif key == ord('o'):
        label = "occupied"
        print(f"Switched to: {label}")
    elif key == ord('e'):
        label = "empty"
        print(f"Switched to: {label}")
    elif key == ord('q'):
        break

cap.release()
cv2.destroyAllWindows()
print(f"\nDone. Total saved: {count}")