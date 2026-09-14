import argparse
import os
import sys
import urllib
import urllib.request
import time
import traceback
import math
import numpy as np
import cv2
from rknn.api import RKNN

# ===================== 全局配置 =====================
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
# 量化校准数据集（rknn_model_zoo 自带 COCO 子集，20 张）
DATASET_PATH = '/home/yzy/aicode/rknn_model_zoo-main/datasets/COCO/coco_subset_20.txt'
MODEL_DIR = os.path.join(BASE_DIR, 'model') + os.sep
MODEL_PATH = MODEL_DIR + 'yolov8n-pose.onnx'
OUT_RKNN_PATH = MODEL_DIR + 'yolov8n_pose.rknn'
TEST_IMG_PATH = os.path.join(BASE_DIR, 'model', 'bus.jpg')

RKNPU1_TARGET = ['rk1808', 'rv1109', 'rv1126']

# YOLOv8-Pose 检测参数
CLASSES = ['person']
nmsThresh = 0.4
objectThresh = 0.5
INPUT_SIZE = (640, 640)

# 关键点可视化配色
pose_palette = np.array([
    [255, 128, 0], [255, 153, 51], [255, 178, 102], [230, 230, 0], [255, 153, 255],
    [153, 204, 255], [255, 102, 255], [255, 51, 255], [102, 178, 255], [51, 153, 255],
    [255, 153, 153], [255, 102, 102], [255, 51, 51], [153, 255, 153], [102, 255, 102],
    [51, 255, 51], [0, 255, 0], [0, 0, 255], [255, 0, 0], [255, 255, 255]
], dtype=np.uint8)
kpt_color = pose_palette[[16, 16, 16, 16, 16, 0, 0, 0, 0, 0, 0, 9, 9, 9, 9, 9, 9]]
skeleton = [
    [16, 14], [14, 12], [17, 15], [15, 13], [12, 13], [6, 12], [7, 13], [6, 7], [6, 8],
    [7, 9], [8, 10], [9, 11], [2, 3], [1, 2], [1, 3], [2, 4], [3, 5], [4, 6], [5, 7]
]
limb_color = pose_palette[[9, 9, 9, 9, 7, 7, 7, 0, 0, 0, 0, 0, 16, 16, 16, 16, 16, 16, 16]]


# ===================== 下载进度工具 =====================
def readable_speed(speed):
    speed_bytes = float(speed)
    speed_kbytes = speed_bytes / 1024
    if speed_kbytes > 1024:
        speed_mbytes = speed_kbytes / 1024
        if speed_mbytes > 1024:
            speed_gbytes = speed_mbytes / 1024
            return "{:.2f} GB/s".format(speed_gbytes)
        else:
            return "{:.2f} MB/s".format(speed_mbytes)
    else:
        return "{:.2f} KB/s".format(speed_kbytes)


def show_progress(blocknum, blocksize, totalsize):
    speed = (blocknum * blocksize) / (time.time() - start_time)
    speed_str = " Speed: {}".format(readable_speed(speed))
    recv_size = blocknum * blocksize

    f = sys.stdout
    progress = (recv_size / totalsize)
    progress_str = "{:.2f}%".format(progress * 100)
    n = round(progress * 50)
    s = ('#' * n).ljust(50, '-')
    f.write(progress_str.ljust(8, ' ') + '[' + s + ']' + speed_str)
    f.flush()
    f.write('\r\n')


def check_and_download_origin_model():
    global start_time
    if not os.path.exists(MODEL_PATH):
        print('--> Download {}'.format(MODEL_PATH))
        url = 'https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/Yolov8/yolov8n-pose.onnx'
        download_file = MODEL_PATH
        try:
            start_time = time.time()
            urllib.request.urlretrieve(url, download_file, show_progress)
        except:
            print('Download {} failed.'.format(download_file))
            print(traceback.format_exc())
            exit(-1)
        print('done')


# ===================== 图像预处理 =====================
def letterbox_resize(image, size, bg_color):
    if isinstance(image, str):
        image = cv2.imread(image)

    target_width, target_height = size
    image_height, image_width, _ = image.shape

    aspect_ratio = min(target_width / image_width, target_height / image_height)
    new_width = int(image_width * aspect_ratio)
    new_height = int(image_height * aspect_ratio)

    image = cv2.resize(image, (new_width, new_height), interpolation=cv2.INTER_AREA)

    result_image = np.ones((target_height, target_width, 3), dtype=np.uint8) * bg_color
    offset_x = (target_width - new_width) // 2
    offset_y = (target_height - new_height) // 2
    result_image[offset_y:offset_y + new_height, offset_x:offset_x + new_width] = image
    return result_image, aspect_ratio, offset_x, offset_y


# ===================== 后处理工具 =====================
class DetectBox:
    def __init__(self, classId, score, xmin, ymin, xmax, ymax, keypoint):
        self.classId = classId
        self.score = score
        self.xmin = xmin
        self.ymin = ymin
        self.xmax = xmax
        self.ymax = ymax
        self.keypoint = keypoint


def IOU(xmin1, ymin1, xmax1, ymax1, xmin2, ymin2, xmax2, ymax2):
    xmin = max(xmin1, xmin2)
    ymin = max(ymin1, ymin2)
    xmax = min(xmax1, xmax2)
    ymax = min(ymax1, ymax2)

    innerWidth = xmax - xmin
    innerHeight = ymax - ymin
    innerWidth = innerWidth if innerWidth > 0 else 0
    innerHeight = innerHeight if innerHeight > 0 else 0
    innerArea = innerWidth * innerHeight

    area1 = (xmax1 - xmin1) * (ymax1 - ymin1)
    area2 = (xmax2 - xmin2) * (ymax2 - ymin2)
    total = area1 + area2 - innerArea
    return innerArea / total


def NMS(detectResult):
    predBoxs = []
    sort_detectboxs = sorted(detectResult, key=lambda x: x.score, reverse=True)

    for i in range(len(sort_detectboxs)):
        xmin1 = sort_detectboxs[i].xmin
        ymin1 = sort_detectboxs[i].ymin
        xmax1 = sort_detectboxs[i].xmax
        ymax1 = sort_detectboxs[i].ymax
        classId = sort_detectboxs[i].classId

        if sort_detectboxs[i].classId != -1:
            predBoxs.append(sort_detectboxs[i])
            for j in range(i + 1, len(sort_detectboxs), 1):
                if classId == sort_detectboxs[j].classId:
                    xmin2 = sort_detectboxs[j].xmin
                    ymin2 = sort_detectboxs[j].ymin
                    xmax2 = sort_detectboxs[j].xmax
                    ymax2 = sort_detectboxs[j].ymax
                    iou = IOU(xmin1, ymin1, xmax1, ymax1, xmin2, ymin2, xmax2, ymax2)
                    if iou > nmsThresh:
                        sort_detectboxs[j].classId = -1
    return predBoxs


def sigmoid(x):
    return 1 / (1 + np.exp(-x))


def softmax(x, axis=-1):
    exp_x = np.exp(x - np.max(x, axis=axis, keepdims=True))
    return exp_x / np.sum(exp_x, axis=axis, keepdims=True)


def process(out, keypoints, index, model_w, model_h, stride, scale_w=1, scale_h=1):
    xywh = out[:, :64, :]
    conf = sigmoid(out[:, 64:, :])
    res = []
    for h in range(model_h):
        for w in range(model_w):
            for c in range(len(CLASSES)):
                if conf[0, c, (h * model_w) + w] > objectThresh:
                    xywh_ = xywh[0, :, (h * model_w) + w]
                    xywh_ = xywh_.reshape(1, 4, 16, 1)
                    data = np.array([i for i in range(16)]).reshape(1, 1, 16, 1)
                    xywh_ = softmax(xywh_, 2)
                    xywh_ = np.multiply(data, xywh_)
                    xywh_ = np.sum(xywh_, axis=2, keepdims=True).reshape(-1)

                    xywh_temp = xywh_.copy()
                    xywh_temp[0] = (w + 0.5) - xywh_[0]
                    xywh_temp[1] = (h + 0.5) - xywh_[1]
                    xywh_temp[2] = (w + 0.5) + xywh_[2]
                    xywh_temp[3] = (h + 0.5) + xywh_[3]

                    xywh_[0] = ((xywh_temp[0] + xywh_temp[2]) / 2)
                    xywh_[1] = ((xywh_temp[1] + xywh_temp[3]) / 2)
                    xywh_[2] = (xywh_temp[2] - xywh_temp[0])
                    xywh_[3] = (xywh_temp[3] - xywh_temp[1])
                    xywh_ = xywh_ * stride

                    xmin = (xywh_[0] - xywh_[2] / 2) * scale_w
                    ymin = (xywh_[1] - xywh_[3] / 2) * scale_h
                    xmax = (xywh_[0] + xywh_[2] / 2) * scale_w
                    ymax = (xywh_[1] + xywh_[3] / 2) * scale_h
                    keypoint = keypoints[..., (h * model_w) + w + index]
                    keypoint[..., 0:2] = keypoint[..., 0:2] // 1
                    box = DetectBox(c, conf[0, c, (h * model_w) + w], xmin, ymin, xmax, ymax, keypoint)
                    res.append(box)
    return res


def postprocess_and_draw(results, img, aspect_ratio, offset_x, offset_y, collect=False):
    outputs = []
    keypoints = results[3]
    for x in results[:3]:
        index, stride = 0, 0
        if x.shape[2] == 20:
            stride = 32
            index = 20 * 4 * 20 * 4 + 20 * 2 * 20 * 2
        if x.shape[2] == 40:
            stride = 16
            index = 20 * 4 * 20 * 4
        if x.shape[2] == 80:
            stride = 8
            index = 0
        feature = x.reshape(1, 65, -1)
        output = process(feature, keypoints, index, x.shape[3], x.shape[2], stride)
        outputs = outputs + output

    predbox = NMS(outputs)

    dets = []   # 新增：收集原图坐标系下的 (box, 17x3关键点)，供 --dump 导出
    for i in range(len(predbox)):
        xmin = int((predbox[i].xmin - offset_x) / aspect_ratio)
        ymin = int((predbox[i].ymin - offset_y) / aspect_ratio)
        xmax = int((predbox[i].xmax - offset_x) / aspect_ratio)
        ymax = int((predbox[i].ymax - offset_y) / aspect_ratio)
        classId = predbox[i].classId
        score = predbox[i].score
        cv2.rectangle(img, (xmin, ymin), (xmax, ymax), (0, 255, 0), 2)
        title = CLASSES[classId] + " %.2f" % score
        cv2.putText(img, title, (xmin, ymin), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2, cv2.LINE_AA)

        kpts = predbox[i].keypoint.reshape(-1, 3)
        kpts[..., 0] = (kpts[..., 0] - offset_x) / aspect_ratio
        kpts[..., 1] = (kpts[..., 1] - offset_y) / aspect_ratio

        if collect:
            dets.append(([xmin, ymin, xmax, ymax], kpts.copy()))

        for k, keypoint in enumerate(kpts):
            x, y, conf = keypoint
            color_k = [int(x) for x in kpt_color[k]]
            if x != 0 and y != 0:
                cv2.circle(img, (int(x), int(y)), 5, color_k, -1, lineType=cv2.LINE_AA)

        for k, sk in enumerate(skeleton):
            pos1 = (int(kpts[(sk[0] - 1), 0]), int(kpts[(sk[0] - 1), 1]))
            pos2 = (int(kpts[(sk[1] - 1), 0]), int(kpts[(sk[1] - 1), 1]))
            if pos1[0] == 0 or pos1[1] == 0 or pos2[0] == 0 or pos2[1] == 0:
                continue
            cv2.line(img, pos1, pos2, [int(x) for x in limb_color[k]], thickness=2, lineType=cv2.LINE_AA)

    return img, len(predbox), dets


# ===================== 主流程 =====================
if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='YOLOv8n-Pose Python Demo (Full Feature)', add_help=True)
    parser.add_argument('--target', type=str,
                        default='rk3566', help='RKNPU target platform')
    parser.add_argument('--npu_device_test', action='store_true',
                        default=False, help='Connected npu device run')
    parser.add_argument('--accuracy_analysis', action='store_true',
                        default=False, help='Accuracy analysis')
    parser.add_argument('--eval_perf', action='store_true',
                        default=False, help='Time consuming evaluation')
    parser.add_argument('--eval_memory', action='store_true',
                        default=False, help='Memory evaluation')
    parser.add_argument('--model', type=str,
                        default=MODEL_PATH, help='onnx model path')
    parser.add_argument('--output_path', type=str,
                        default=OUT_RKNN_PATH, help='output rknn model path')
    parser.add_argument('--dtype', type=str, default='i8',
                        help='dtype of model, i8/fp32 for RKNPU2, u8/fp32 for RKNPU1')
    parser.add_argument('--skip_post', action='store_true',
                        default=False, help='Skip pose postprocess/draw (for perf/memory analysis of other models)')
    # ===================== 新增：任意输入 + 检测结果导出 =====================
    parser.add_argument('--input', type=str,
                        default=TEST_IMG_PATH, help='输入图片/视频路径（默认 bus.jpg）')
    parser.add_argument('--dump', type=str,
                        default='', help='导出每个人的 (框+17关键点) 到 .npz，供 calibrate_pose.py 离线标定')
    args = parser.parse_args()

    # 只有 onnx 转换才自动下载原始模型；直接加载 .rknn 时跳过
    model_suffix = os.path.splitext(args.model)[1]
    if model_suffix != ".rknn":
        check_and_download_origin_model()

    # 创建 RKNN 对象
    rknn = RKNN(verbose=False)

    # 区分是rknn模型(直接推理)还是onnx(需要转换)
    if model_suffix == ".rknn":
        print("--> Direct load exist rknn model, skip convert process")
        ret = rknn.load_rknn(args.model)
        if ret != 0:
            print('Load rknn model failed!')
            exit(ret)
        print('done')
    else:
        # ========== 原有转换流程完整保留 ==========
        print('--> Config model')
        rknn.config(
            mean_values=[[0, 0, 0]],
            std_values=[[255, 255, 255]],
            target_platform=args.target
        )
        print('done')

        print('--> Loading onnx model')
        ret = rknn.load_onnx(model=args.model)
        if ret != 0:
            print('Load onnx model failed!')
            exit(ret)
        print('done')

        print('--> Building model')
        do_quant = True if (args.dtype == 'i8' or args.dtype == 'u8') else False
        if args.target in RKNPU1_TARGET:
            ret = rknn.build(do_quantization=do_quant, dataset=DATASET_PATH, auto_hybrid_quant=True)
        else:
            # if do_quant:
            #     rknn.hybrid_quantization_step1(
            #         dataset=DATASET_PATH,
            #         proposal=False,
            #         custom_hybrid=[
            #             ['/model.22/cv4.0/cv4.0.0/act/Mul_output_0', '/model.22/Concat_6_output_0'],
            #             ['/model.22/cv4.1/cv4.1.0/act/Mul_output_0', '/model.22/Concat_6_output_0'],
            #             ['/model.22/cv4.2/cv4.2.0/act/Mul_output_0', '/model.22/Concat_6_output_0']
            #         ]
            #     )
            #     model_name = os.path.basename(args.model).replace('.onnx', '')
            #     rknn.hybrid_quantization_step2(
            #         model_input=model_name + ".model",
            #         data_input=model_name + ".data",
            #         model_quantization_cfg=model_name + ".quantization.cfg"
            #     )
            #     ret = 0
            # else:
            #     ret = rknn.build(do_quantization=do_quant, dataset=DATASET_PATH)
            ret = rknn.build(do_quantization=do_quant, dataset=DATASET_PATH)
        if ret != 0:
            print('Build model failed!')
            exit(ret)
        print('done')

        print('--> Export rknn model')
        ret = rknn.export_rknn(args.output_path)
        if ret != 0:
            print('Export rknn model failed!')
            exit(ret)
        print('done')

    # 准备输入（图片或视频）
    is_video = args.input.lower().endswith(('.mp4', '.avi', '.mkv', '.mov', '.h264', '.h265'))
    cap = None
    if is_video:
        cap = cv2.VideoCapture(args.input)
        if not cap.isOpened():
            print('打开视频失败: {}'.format(args.input))
            exit(-1)
        ret, img = cap.read()
        if not ret:
            print('读取视频首帧失败')
            exit(-1)
    else:
        img = cv2.imread(args.input)
        if img is None:
            print('读取图片失败: {}'.format(args.input))
            exit(-1)

    letterbox_img, aspect_ratio, offset_x, offset_y = letterbox_resize(img, INPUT_SIZE, 56) #建议114
    infer_img = letterbox_img[..., ::-1]  # BGR -> RGB

    # Init runtime environment
    print('--> Init runtime environment')
    # 只要开启性能/内存评估，两个参数都要打开
    need_debug = args.eval_perf or args.eval_memory
    if args.npu_device_test or args.target in RKNPU1_TARGET:
        if need_debug:
            ret = rknn.init_runtime(target=args.target, perf_debug=True, eval_mem=True)
        else:
            ret = rknn.init_runtime(target=args.target)
    elif need_debug:
        ret = rknn.init_runtime(
            target=args.target, perf_debug=True, eval_mem=True)
    else:
        if args.target in RKNPU1_TARGET:
            print('The target {} does not support simulator.'.format(args.target))
            print('Please set `--npu_device_test` to init runtime with real target.')
            exit(-1)
        ret = rknn.init_runtime()
    if ret != 0:
        print('Init runtime environment failed!')
        exit(ret)
    print('done')

    # 性能耗时评估
    if args.eval_perf:
        print('--> Eval Perf')
        rknn.eval_perf()
        print('done')

    # 内存占用评估
    if args.eval_memory:
        print('--> Eval Memory')
        rknn.eval_memory()
        print('done')

    # 推理（图片单帧 / 视频逐帧）
    all_boxes, all_kpts = [], []
    writer = None
    frame_idx = 0

    def infer_frame(frame):
        lb, ar, ox, oy = letterbox_resize(frame, INPUT_SIZE, 56)
        outs = rknn.inference(inputs=[lb[..., ::-1]])
        return outs, ar, ox, oy

    print('--> Running model')
    while True:
        outputs, ar, ox, oy = infer_frame(img)

        if args.skip_post:
            print('--> Skip PostProcess & Draw (--skip_post)')
        else:
            result_img, det_count, dets = postprocess_and_draw(
                outputs, img.copy(), ar, ox, oy, collect=bool(args.dump))
            if args.dump:
                for (b, k) in dets:
                    all_boxes.append(b)
                    all_kpts.append(k)

            if is_video:
                if writer is None:
                    fourcc = cv2.VideoWriter_fourcc(*'mp4v')
                    writer = cv2.VideoWriter(args.input + '.out.mp4', fourcc, 10.0,
                                             (result_img.shape[1], result_img.shape[0]))
                writer.write(result_img)
                print('Frame {}: {} persons'.format(frame_idx, det_count))
            else:
                cv2.imwrite("./1.jpg", result_img)
                print('Detected {} persons. Result saved to ./1.jpg'.format(det_count))
            print('done')

        if is_video:
            ret, img = cap.read()
            if not ret:
                break
            frame_idx += 1
        else:
            break

    if writer is not None:
        writer.release()
    if cap is not None:
        cap.release()

    # 导出检测结果（供 calibrate_pose.py 离线标定姿态阈值）
    if args.dump and all_boxes:
        np.savez(args.dump,
                 boxes=np.array(all_boxes, dtype=np.int32),
                 keypoints=np.array(all_kpts, dtype=np.float32))
        print('--> Dump {} persons to {}'.format(len(all_boxes), args.dump))

    # 精度分析
    if args.accuracy_analysis:
        print('--> Accuracy analysis')
        if args.npu_device_test:
            ret = rknn.accuracy_analysis(inputs=[TEST_IMG_PATH], target=args.target)
        else:
            ret = rknn.accuracy_analysis(inputs=[TEST_IMG_PATH])
        if ret != 0:
            print('Accuracy analysis failed!')
            exit(ret)
        print('done')

    # 释放资源
    rknn.release()