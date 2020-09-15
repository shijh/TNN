package com.tencent.tnn.demo.ImageBlazeFaceDetector;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.View;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.ToggleButton;

import com.tencent.tnn.demo.BlazeFaceDetector;
import com.tencent.tnn.demo.FileUtils;
import com.tencent.tnn.demo.Helper;
import com.tencent.tnn.demo.R;
import com.tencent.tnn.demo.common.component.DrawView;
import com.tencent.tnn.demo.common.fragment.BaseFragment;

import java.util.ArrayList;


public class ImageBlazeFaceDetectFragment extends BaseFragment {

    private final static String TAG = BlazeFaceDetector.class.getSimpleName();
    private BlazeFaceDetector mFaceDetector = new BlazeFaceDetector();

    private static final String IMAGE = "test_blazeface.jpg";
    private static final int NET_H_INPUT = 128;
    private static final int NET_W_INPUT = 128;
    private Paint mPaint = new Paint();
    private DrawView mDrawView;
    private ToggleButton mGPUSwitch;
    private Button mRunButton;
    private boolean mUseGPU = false;
    //add for npu
    private ToggleButton mHuaweiNPUswitch;
    private boolean mUseHuaweiNpu = false;
    private TextView HuaweiNpuTextView;

    /**********************************     Get Preview Advised    **********************************/

    @Override
    public void onCreate(Bundle savedInstanceState) {
        Log.d(TAG, "onCreate");
        super.onCreate(savedInstanceState);
        System.loadLibrary("tnn_wrapper");
        String modelPath = initModel();
        NpuEnable = mFaceDetector.checkNpu(modelPath);
    }

    private String initModel() {

        String targetDir = getActivity().getFilesDir().getAbsolutePath();

        //copy detect model to sdcard
        String[] modelPathsDetector = {
                "blazeface.tnnmodel",
                "blazeface.tnnproto",
        };

        for (int i = 0; i < modelPathsDetector.length; i++) {
            String modelFilePath = modelPathsDetector[i];
            String interModelFilePath = targetDir + "/" + modelFilePath;
            FileUtils.copyAsset(getActivity().getAssets(), "blazeface/" + modelFilePath, interModelFilePath);
        }
        FileUtils.copyAsset(getActivity().getAssets(),"blazeface_anchors.txt", targetDir + "/blazeface_anchors.txt");
        return targetDir;
    }

    @Override
    public void onClick(View view) {
        int i = view.getId();
        if (i == R.id.back_rl) {
            clickBack();
        }
    }

    private void onSwichGPU(boolean b) {
        if (b && mHuaweiNPUswitch.isChecked()) {
            mHuaweiNPUswitch.setChecked(false);
            mUseHuaweiNpu = false;
        }
        mUseGPU = b;
        TextView result_view = (TextView) $(R.id.result);
        result_view.setText("");
    }

    private void onSwichNPU(boolean b) {
        if (b && mGPUSwitch.isChecked()) {
            mGPUSwitch.setChecked(false);
            mUseGPU = false;
        }
        mUseHuaweiNpu = b;
        TextView result_view = (TextView) $(R.id.result);
        result_view.setText("");
    }

    private void clickBack() {
        if (getActivity() != null) {
            (getActivity()).finish();
        }
    }

    @Override
    public void setFragmentView() {
        Log.d(TAG, "setFragmentView");
        setView(R.layout.fragment_image_detector);
        setTitleGone();
        $$(R.id.back_rl);
        $$(R.id.gpu_switch);
        mGPUSwitch = $(R.id.gpu_switch);
        mGPUSwitch.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton compoundButton, boolean b) {
                onSwichGPU(b);
            }
        });

        $$(R.id.npu_switch);
        mHuaweiNPUswitch = $(R.id.npu_switch);
        mHuaweiNPUswitch.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton compoundButton, boolean b) {
                onSwichNPU(b);
            }
        });

        HuaweiNpuTextView = $(R.id.npu_text);

        if (!NpuEnable) {
            HuaweiNpuTextView.setVisibility(View.INVISIBLE);
            mHuaweiNPUswitch.setVisibility(View.INVISIBLE);
        }
        mDrawView = (DrawView) $(R.id.drawView);
        mRunButton = $(R.id.run_button);
        mRunButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                startDetect();
            }
        });

        Bitmap originBitmap = FileUtils.readBitmapFromFile(getActivity().getAssets(), IMAGE);
        ImageView source = (ImageView) $(R.id.origin);
        source.setImageBitmap(originBitmap);
    }

    @Override
    public void openCamera() {

    }

    @Override
    public void startPreview(SurfaceHolder surfaceHolder) {

    }

    @Override
    public void closeCamera() {

    }


    private void startDetect() {

        Bitmap originBitmap = FileUtils.readBitmapFromFile(getActivity().getAssets(), IMAGE);
        Bitmap scaleBitmap = Bitmap.createScaledBitmap(originBitmap, NET_W_INPUT, NET_H_INPUT, false);
        ImageView source = (ImageView) $(R.id.origin);
        source.setImageBitmap(originBitmap);
        String modelPath = initModel();
        Log.d(TAG, "Init classify " + modelPath);
        int device = 0;
        if (mUseHuaweiNpu) {
            device = 2;
        } else if (mUseGPU) {
            device = 1;
        }
        int result = mFaceDetector.init(modelPath, NET_W_INPUT, NET_H_INPUT, 0.7f, 0.3f, -1, device);
        Log.d(TAG, "detect from image");
        BlazeFaceDetector.BlazeFaceInfo[] faceInfoList = mFaceDetector.detectFromImage(originBitmap, originBitmap.getWidth(), originBitmap.getHeight());
        int faceCount = 0;
        if (faceInfoList != null) {
            faceCount = faceInfoList.length;
        }
        mPaint.setStyle(Paint.Style.STROKE);
        Bitmap scaleBitmap2 = originBitmap.copy(Bitmap.Config.ARGB_8888, true);
        Canvas canvas = new Canvas(scaleBitmap2);
        ArrayList<Rect> rects = new ArrayList<Rect>();

        for (int i = 0; i < faceInfoList.length; i++) {
            BlazeFaceDetector.BlazeFaceInfo tmpFaceInfo = faceInfoList[i];
            rects.add(new Rect((int) (tmpFaceInfo.x1), (int) (tmpFaceInfo.y1), (int) (tmpFaceInfo.x2), (int) (tmpFaceInfo.y2)));
            for (int j = 0; j < tmpFaceInfo.keypoints.length; j++) {
                mPaint.setARGB(255, 0, 255, 0);
                mPaint.setStrokeWidth(5);
                canvas.drawPoint(tmpFaceInfo.keypoints[j][0], tmpFaceInfo.keypoints[j][1], mPaint);
            }
        }
        mPaint.setStrokeWidth(8);
        for (int i = 0; i < rects.size(); i++) {
            Log.d(TAG, "rect " + rects.get(i));
            Rect rect = rects.get(i);
            mPaint.setARGB(255, 0, 255, 0);
            canvas.drawRect(rect, mPaint);
        }
        source.setImageBitmap(scaleBitmap2);
        source.draw(canvas);
        String benchResult = "face count: "  + faceCount+ " " + Helper.getBenchResult();
        TextView result_view = (TextView) $(R.id.result);
        result_view.setText(benchResult);
    }

    @Override
    public void onStart() {
        Log.d(TAG, "onStart");
        super.onStart();
    }

    @Override
    public void onResume() {
        super.onResume();
        Log.d(TAG, "onResume");

        getFocus();
    }

    @Override
    public void onPause() {
        Log.d(TAG, "onPause");
        super.onPause();
    }

    @Override
    public void onStop() {
        Log.i(TAG, "onStop");
        super.onStop();
    }


    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "onDestroy");
    }

    private void preview() {
        Log.i(TAG, "preview");

    }

    private void getFocus() {
        getView().setFocusableInTouchMode(true);
        getView().requestFocus();
        getView().setOnKeyListener(new View.OnKeyListener() {
            @Override
            public boolean onKey(View v, int keyCode, KeyEvent event) {
                if (event.getAction() == KeyEvent.ACTION_UP && keyCode == KeyEvent.KEYCODE_BACK) {
                    clickBack();
                    return true;
                }
                return false;
            }
        });
    }

}
