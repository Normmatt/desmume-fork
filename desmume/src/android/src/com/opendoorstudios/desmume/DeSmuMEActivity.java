package com.opendoorstudios.desmume;

/*
	Copyright (C) 2012 Jeffrey Quesnelle

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

import java.io.File;
import java.util.Date;
import java.util.Timer;
import java.util.TimerTask;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.app.ProgressDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.DialogInterface.OnCancelListener;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.SharedPreferences.OnSharedPreferenceChangeListener;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Bitmap.Config;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Message;
import android.preference.PreferenceManager;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceHolder.Callback;
import android.view.SurfaceView;
import android.view.WindowManager;

public class DeSmuMEActivity extends Activity implements OnSharedPreferenceChangeListener {

	static Controls controls;
	NDSView view;
	static final String TAG = "desmume";
	Dialog loadingDialog = null;
	
	Handler msgHandler = new Handler() {
		
		@Override
		public
		void dispatchMessage(Message msg) {
			switch(msg.what) {
			case PICK_ROM:
				pickRom();
				break;
			case LOADING_START:
				if(loadingDialog == null) {
					final String loadingMsg = getResources().getString(R.string.loading);
					loadingDialog = ProgressDialog.show(DeSmuMEActivity.this, null, loadingMsg, true);
					break;
				}
				break;
			case LOADING_END:
				if(loadingDialog != null) {
					loadingDialog.dismiss();
					loadingDialog = null;
				}
				break;
			case ROM_ERROR:
				AlertDialog.Builder builder = new AlertDialog.Builder(DeSmuMEActivity.this);
				builder.setMessage(R.string.rom_error).setPositiveButton(R.string.OK, new DialogInterface.OnClickListener() {
					
					@Override
					public void onClick(DialogInterface arg0, int arg1) {
						arg0.dismiss();
						pickRom();
					}
				}).setOnCancelListener(new OnCancelListener() {

					@Override
					public void onCancel(DialogInterface arg0) {
						arg0.dismiss();
						pickRom();
					}
					
				});
				builder.create().show();
			}
		}
		
	};
	
	public static final int PICK_ROM = 1338;
	public static final int LOADING_START = 1339;
	public static final int LOADING_END = 1340;
	public static final int ROM_ERROR = 1341;
	
//	public static boolean IS_OUYA = tv.ouya.console.api.OuyaFacade.getInstance().isRunningOnOUYAHardware();
	public static boolean IS_OUYA = false;
	
	@Override
	public void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		
		getWindow().setFlags(WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED, WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED);
		
		if(IS_OUYA)
			Log.i(TAG, "Starting in OUYA mode");

		view = new NDSView(this);
		setContentView(view);
		
		controls = new Controls(view);

		Settings.applyDefaults(this);
		prefs = PreferenceManager.getDefaultSharedPreferences(DeSmuMEActivity.this);
		prefs.registerOnSharedPreferenceChangeListener(this);
		loadJavaSettings(null);
		
		if (!DeSmuME.inited) {
			DeSmuME.context = this;
			
			final String defaultWorkingDir = Environment.getExternalStorageDirectory().getAbsolutePath() + "/DeSmuME";
			final String path = prefs.getString(Settings.DESMUME_PATH, defaultWorkingDir);
			final File workingDir = new File(path);
			final File tempDir = new File(path + "/Temp");
			tempDir.mkdir();
			DeSmuME.setWorkingDir(workingDir.getAbsolutePath(), tempDir.getAbsolutePath() + "/");
			workingDir.mkdir();
			new File(path + "/States").mkdir();
			new File(path + "/Battery").mkdir();
			new File(path + "/Cheats").mkdir();
			
			//clear any previously extracted ROMs
			
			final File[] cacheFiles = tempDir.listFiles();
			if(cacheFiles != null) {
				for(File cacheFile : cacheFiles) {
					if(cacheFile.getAbsolutePath().toLowerCase().endsWith(".nds"))
						cacheFile.delete();
				}
			}
			
			DeSmuME.init();
			DeSmuME.inited = true;
			
			pickRom();
		}
	}
	
	@Override
	public void onConfigurationChanged(Configuration newConfig) {
	    super.onConfigurationChanged(newConfig);
	    setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
	}
	
	void runEmulation() {
		if (DeSmuME.inited) {
			DeSmuME.unpause();
		}
	}
	
	void pauseEmulation() {
		if (DeSmuME.inited) {
			DeSmuME.pause();
		}
	}
	
	void pickRom() {
		Intent i = new Intent(this, FileDialog.class);
		i.setAction(Intent.ACTION_PICK);
		
		String startPath = Environment.getExternalStorageDirectory().getPath();
		final File path = new File(prefs.getString(Settings.LAST_ROM_DIR, startPath));
		if(path.exists()) //make sure this path actually exists -- otherwise default back to /mnt/sdcard/
			startPath = path.getPath();
		
		i.putExtra(FileDialog.START_PATH, startPath);
		i.putExtra(FileDialog.FORMAT_FILTER, new String[] {".nds", ".zip", ".7z", ".rar"});
		startActivityForResult(i, PICK_ROM);
	}
	
	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		if(requestCode != PICK_ROM || resultCode != Activity.RESULT_OK)
			return;
		String romPath = data.getStringExtra(FileDialog.RESULT_PATH);
		if(romPath != null) {
			final File romDir = new File(romPath);
			prefs.edit().putString(Settings.LAST_ROM_DIR, romDir.getParent()).apply();
			
			msgHandler.sendEmptyMessage(DeSmuMEActivity.LOADING_START);
			
			DeSmuME.loadRom(romPath);
		}
			
	}
	
	@Override
	public void onResume() {
		super.onResume();
		runEmulation();
		startDrawTimer();
	}
	
	@Override
	public void onPause() {
		super.onPause();
		pauseEmulation();
		stopDrawTimer();
	}
	
	Timer drawtimer;
	void startDrawTimer() {
		drawtimer = new Timer();
		drawtimer.schedule(new TimerTask() {
			public void run(){
				view.postInvalidate();
			}
		}, 16, 16);
	}
	
	void stopDrawTimer() {
		if (drawtimer != null)
			drawtimer.cancel();
	}
	
	@Override
	public boolean onCreateOptionsMenu(Menu menu) {
		MenuInflater inflater = getMenuInflater();
	    inflater.inflate(R.menu.activity_main, menu);
	    return true;
	}
	
	@Override
	public boolean onPrepareOptionsMenu(Menu menu) {
		pauseEmulation();
		menu.findItem(R.id.cheats).setVisible(DeSmuME.romLoaded);
		menu.findItem(R.id.lid).setChecked(DeSmuME.lidOpen);
		
		final String defaultWorkingDir = Environment.getExternalStorageDirectory().getAbsolutePath() + "/nds4droid";
		final String statesPath = prefs.getString(Settings.DESMUME_PATH, defaultWorkingDir) + "/States/";
		
		String romSaveName = DeSmuME.loadedRom != null ? new File(DeSmuME.loadedRom).getName() : null;
		if(romSaveName != null)
			romSaveName = romSaveName.substring(0, romSaveName.lastIndexOf('.'));
		
		for(int i = 1 ; i <= 9 ; ++i) {
			
			MenuItem saveItem = null, loadItem = null;
			switch(i) {
			case 1: saveItem = menu.findItem(R.id.save1); loadItem = menu.findItem(R.id.restore1); break;
			case 2: saveItem = menu.findItem(R.id.save2); loadItem = menu.findItem(R.id.restore2); break;
			case 3: saveItem = menu.findItem(R.id.save3); loadItem = menu.findItem(R.id.restore3); break;
			case 4: saveItem = menu.findItem(R.id.save4); loadItem = menu.findItem(R.id.restore4); break;
			case 5: saveItem = menu.findItem(R.id.save5); loadItem = menu.findItem(R.id.restore5); break;
			case 6: saveItem = menu.findItem(R.id.save6); loadItem = menu.findItem(R.id.restore6); break;
			case 7: saveItem = menu.findItem(R.id.save7); loadItem = menu.findItem(R.id.restore7); break;
			case 8: saveItem = menu.findItem(R.id.save8); loadItem = menu.findItem(R.id.restore8); break;
			case 9: saveItem = menu.findItem(R.id.save9); loadItem = menu.findItem(R.id.restore9); break;
			}
			
			String newTitle = String.valueOf(i);
			
			if(romSaveName != null) {
				final File thisSave = new File(statesPath + romSaveName + ".ds" + i);
				if(thisSave.exists()) 
					newTitle = String.format("%d - %s", i, new Date(thisSave.lastModified()).toString());
			}
			
			saveItem.setTitle(newTitle);
			loadItem.setTitle(newTitle);
		}
		
		return true;
	}
	
	@Override
	public void onOptionsMenuClosed(Menu menu) {
		runEmulation();
	}
	
	@Override
	public boolean onMenuItemSelected (int featureId, MenuItem item) {
		switch(item.getItemId()) {
		case R.id.load:
			pickRom();
			break;
		case R.id.quicksave:
			saveState(0);
			break;
		case R.id.quickrestore:
			restoreState(0);
			break;
		case R.id.restore1: case R.id.restore2: case R.id.restore3: case R.id.restore4: case R.id.restore5:
		case R.id.restore6: case R.id.restore7: case R.id.restore8: case R.id.restore9:
		{
			String title = item.getTitle().toString();
			final int spacePos = title.indexOf(' ');
			if(spacePos != -1)
				title = title.substring(0, spacePos);
			restoreState(Integer.valueOf(title));
		}
			break;
		case R.id.save1: case R.id.save2: case R.id.save3: case R.id.save4: case R.id.save5:
		case R.id.save6: case R.id.save7: case R.id.save8: case R.id.save9:
		{
			String title = item.getTitle().toString();
			final int spacePos = title.indexOf(' ');
			if(spacePos != -1)
				title = title.substring(0, spacePos);
			saveState(Integer.valueOf(title));
		}
			break;
		case R.id.settings:
			startActivity(new Intent(this, Settings.class));
			break;
		case R.id.cheats:
			startActivity(new Intent(this, Cheats.class));
			break;
		case R.id.exit:
			DeSmuME.exit();
			finish();
			break;
		case R.id.lid:
		{
			boolean newState = !DeSmuME.lidOpen;
			DeSmuME.lidOpen = newState;
			item.setChecked(newState);
		}
			break;
		default:
			return false;
		}
		runEmulation();
		return true;
	}
	
	void restoreState(int slot) {
		if(DeSmuME.romLoaded)
			DeSmuME.restoreState(slot);
	}
	
	void saveState(int slot) {
		if(DeSmuME.romLoaded)
			DeSmuME.saveState(slot);
	}

	
	SharedPreferences prefs = null;
	
	@Override
	public void onSharedPreferenceChanged(SharedPreferences sharedPreferences, String key) {
		if(DeSmuME.inited)
			loadJavaSettings(key);
	}
	
	void loadJavaSettings(String key)
	{
		if(view != null) {
			view.showfps = prefs.getBoolean(Settings.SHOW_FPS, false);
			view.lcdSwap = prefs.getBoolean(Settings.LCD_SWAP, false);
			view.buttonAlpha = (int)(prefs.getInt(Settings.BUTTON_TRANSPARENCY, 78) * 2.55f);
			view.haptic = prefs.getBoolean(Settings.HAPTIC, false);
			view.dontRotate = prefs.getBoolean(Settings.DONT_ROTATE_LCDS, false);
			view.alwaysTouch = prefs.getBoolean(Settings.ALWAYS_TOUCH, false);
			
			controls.loadMappings(this);
			
			if(key != null) {
				if(key.equals(Settings.SCREEN_FILTER)) {
					int newFilter = DeSmuME.getSettingInt(Settings.SCREEN_FILTER, 0);
					DeSmuME.setFilter(newFilter);
					view.forceResize();
				}
				else if(key.equals(Settings.RENDERER)) {
					int new3D = DeSmuME.getSettingInt(Settings.RENDERER, 2);
					DeSmuME.change3D(new3D);
				}
				else if(key.equals(Settings.SOUNDCORE)) {
					int newSound = DeSmuME.getSettingInt(Settings.SOUNDCORE, 0);
					DeSmuME.changeSound(newSound);
				}
				else if(key.equals(Settings.SYNCHMODE)) {
					int newSynchMode = DeSmuME.getSettingInt(Settings.SYNCHMODE, 0);
					DeSmuME.changeSoundSynchMode(newSynchMode);
				}
				else if(key.equals(Settings.SYNCHMETHOD)) {
					int newSynchMethod = DeSmuME.getSettingInt(Settings.SYNCHMETHOD, 0);
					DeSmuME.changeSoundSynchMethod(newSynchMethod);
				}
				else if(key.equals(Settings.CPU_MODE)) {
					int newCpuMode = DeSmuME.getSettingInt(Settings.CPU_MODE, 1);
					DeSmuME.changeCpuMode(newCpuMode);
				}
			}
		}
	}
	
	
	class NDSView extends SurfaceView implements Callback {
		Bitmap emuBitmap, emuBitmapTouch;
		
		final Paint emuPaint = new Paint();
		final Paint hudPaint = new Paint();
		final float defhudsize = 15;
		float curhudsize = defhudsize;
		
		public boolean showfps = false;
		public boolean lcdSwap = false;
		public boolean forceTouchScreen = false;
		public int buttonAlpha = 78;
		public boolean haptic = true;
		public boolean alwaysTouch = false;
		
		public NDSView(Context context) {
			super(context);
			getHolder().addCallback(this);
			setKeepScreenOn(true);
			setWillNotDraw(false);
			setFocusable(true);
			setFocusableInTouchMode(true);
			
			hudPaint.setColor(Color.GREEN);
			hudPaint.setTextSize(defhudsize);
			hudPaint.setAntiAlias(false);
		}
		
		boolean doForceResize = false;
		public void forceResize() {
			doForceResize = true;
		}
		
		
		
		@Override
		public void onDraw(Canvas canvas) {
			canvas.drawColor(Color.BLACK);
			
			synchronized(view) {
				if(!DeSmuME.inited)
					return;
				
				if(doForceResize)
					view.resize(width, height, pixelFormat);
				
				if(emuBitmap == null)
					return;
				
				int data = DeSmuME.draw(emuBitmap);
				
				final boolean drawTouch = destTouch.left != 0 || destTouch.right != 0;

				if(lcdSwap) {
					canvas.drawBitmap(emuBitmap, srcMain, destMain, emuPaint);
					if(drawTouch)
						canvas.drawBitmap(emuBitmap, srcTouch, destTouch, emuPaint);
				}
				else {
					canvas.drawBitmap(emuBitmap, srcMain, destMain, emuPaint);
					if(drawTouch)
						canvas.drawBitmap(emuBitmap, srcTouch, destTouch, emuPaint);
				}
				
				controls.drawControls(canvas);
				
				if (showfps) {
					int fps = (data >> 24) & 0xFF;
					int fps3d = (data >> 16) & 0xFF;
					int cpuload0 = (data >> 8) & 0xFF;
					int cpuload1 = data & 0xFF;
					
					String hud = "Fps:"+fps+"/"+fps3d+"("+cpuload0+"%/"+cpuload1+"%)";
						
					canvas.drawText(hud, 10, curhudsize, hudPaint);
					
//					if (canvas.isHardwareAccelerated())
//						canvas.drawText("true", 10, curhudsize*2, hudPaint);
				}
			}
		}
		
		
		@Override
		public boolean onTouchEvent(MotionEvent event) {
			return controls.onTouchEvent(event);
		}
		
		boolean resized = false;
		boolean sized = false;
		boolean landscape = false;
		boolean dontRotate = false;
		int sourceWidth;
		int sourceHeight;
		Rect srcMain, destMain, srcTouch, destTouch;
		int width = 0, height = 0, pixelFormat;
		
		
		void resize(int newWidth, int newHeight, int newPixelFormat) {
			
			synchronized(view) {
				sourceWidth = DeSmuME.getNativeWidth();
				sourceHeight = DeSmuME.getNativeHeight();
				resized = true;
				
				final boolean hasScreenFilter = DeSmuME.getSettingInt(Settings.SCREEN_FILTER, 0) != 0;
				final boolean is565 = newPixelFormat == PixelFormat.RGB_565 && !hasScreenFilter;
				final boolean stretch = !prefs.getBoolean(Settings.MAINTAIN_ASPECT_RATIO, false);
				landscape = newWidth > newHeight;
				controls.setView(this);
				int xBlack = 0, yBlack = 0;
				
				forceTouchScreen = !prefs.getBoolean("Controls." + (landscape ? "Landscape." : "Portrait.") + "Draw", false);
				
				if(landscape) {
					if(stretch) {
						destMain = new Rect(0, 0, newWidth / 2, newHeight);
						destTouch = new Rect(newWidth / 2, 0, newWidth, newHeight);
					}
					else {
						final float dsAspect = (float)(sourceWidth * 2) / (float)(sourceHeight / 2);
						final float screenAspect = (float)newWidth / (float)newHeight;
						
						if(dsAspect > screenAspect) {
							//the DS is "wider" than our screen -- match to the width
							final int aspectHeight = (int)((float)newWidth / dsAspect);
							final int black = yBlack = (newHeight - aspectHeight) / 2;
							
							destMain = new Rect(0,black,newWidth / 2,newHeight - black);
							destTouch = new Rect(newWidth / 2,black,newWidth,newHeight-black);
						}
						else {
							//match to the height
							final int aspectWidth = (int)((float)newHeight * dsAspect);
							final int black = xBlack = (newWidth - aspectWidth) / 2;

							destMain = new Rect(black, 0, aspectWidth / 2 + black, newHeight);
							destTouch = new Rect(aspectWidth / 2 + black, 0, newWidth - black, newHeight);
						}
					}
				}
				else {
					if(stretch) {
						destMain = new Rect(0, 0, newWidth, newHeight / 2);
						destTouch = new Rect(0, newHeight / 2, newWidth, newHeight);
					}
					else {
						final float dsAspect = (float)sourceWidth / (float)sourceHeight;
						final float screenAspect = (float)newWidth / (float)newHeight;
						
						if(dsAspect > screenAspect) {
							//the DS is "wider" than our screen -- match to the width
							final int aspectHeight = (int)((float)newWidth / dsAspect);
							final int black = yBlack = (newHeight - aspectHeight) / 2;

							destMain = new Rect(0,black,newWidth,aspectHeight / 2 + black);
							destTouch = new Rect(0,aspectHeight / 2 + black,newWidth,newHeight-black);
						}
						else {
							//match to the height
							final int aspectWidth = (int)((float)newHeight * dsAspect);
							final int black = xBlack = (newWidth - aspectWidth) / 2;

							destMain = new Rect(black, 0, aspectWidth + black, newHeight / 2);
							destTouch = new Rect(black, newHeight / 2, aspectWidth + black, newHeight);
						}
					}
				}
				
				controls.loadControls(DeSmuMEActivity.this, newWidth, newHeight, xBlack, yBlack, is565, landscape);
				
				emuBitmap = Bitmap.createBitmap(sourceWidth, sourceHeight, is565 ? Config.RGB_565 : Config.ARGB_8888);
				srcMain = new Rect(0, 0, sourceWidth, sourceHeight / 2);
				srcTouch = new Rect(0, sourceHeight / 2, sourceWidth, sourceHeight);
				DeSmuME.resize(emuBitmap);
				
				requestFocus();
				
				width = newWidth;
				height = newHeight;
				pixelFormat = newPixelFormat;
				sized = true;
				doForceResize = false;
				
				float max_wh = newWidth > newHeight ? newWidth : newHeight;
				curhudsize = (max_wh / 384.0f) * defhudsize;
				hudPaint.setTextSize(curhudsize);
				
				Log.i(DeSmuMEActivity.TAG, "NDSView resize() newWidth : "+newWidth+" ,newHeight : "+newHeight+", is565 : "+is565);
				Log.i(DeSmuMEActivity.TAG, "NDSView resize() sourceWidth : "+sourceWidth+" ,sourceHeight : "+sourceHeight);
			}
		}


		@Override
		public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
			synchronized(view) {
				view.resize(width, height, format);
			}
		}

		@Override
		public void surfaceCreated(SurfaceHolder arg0) {
		}

		@Override
		public void surfaceDestroyed(SurfaceHolder arg0) {
		}
		
		@Override
		public boolean onKeyDown(int keyCode, KeyEvent event) {
			return controls.onKeyDown(keyCode, event);
		}
		
		@Override
		public boolean onKeyUp(int keyCode, KeyEvent event) {
			return controls.onKeyUp(keyCode, event);
		}
		
		public void showMenu() {
			DeSmuMEActivity.this.openOptionsMenu();
		}
		
	}
	
}
