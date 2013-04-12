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

import android.app.Activity;
import android.app.AlertDialog;
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
	SharedPreferences prefs = null;
	
	static final String TAG = "desmume";

	public static final int PICK_ROM = 1338;
	
	@Override
	public void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		
		getWindow().setFlags(WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED, WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED);
		
		DeSmuME.context = this;
		
		Settings.applyDefaults(this);
		prefs = PreferenceManager.getDefaultSharedPreferences(DeSmuMEActivity.this);
		prefs.registerOnSharedPreferenceChangeListener(this);
		
		if(!DeSmuME.inited) {
			final String defaultWorkingDir = Environment.getExternalStorageDirectory().getAbsolutePath() + "/desmume";
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
			if (cacheFiles != null)
				for(File cacheFile : cacheFiles) {
					if(cacheFile.getAbsolutePath().toLowerCase().endsWith(".nds"))
						cacheFile.delete();
				}
			
			DeSmuME.init();
			DeSmuME.inited = true;
			
			DeSmuME.showfps = prefs.getBoolean(Settings.SHOW_FPS, false);
			
			Log.i(DeSmuMEActivity.TAG, "DeSmuME inited");
		}
		
		view = new NDSView(this);
		setContentView(view);
		
		controls = new Controls(view);
		
		pickRom();
	}
	
	@Override
	public void onBackPressed()
	{
		super.onBackPressed();
		
		DeSmuME.closeRom();
		DeSmuME.exit();
		
		DeSmuME.inited = false;
	}
	
	@Override
	public void onConfigurationChanged(Configuration newConfig) {
	    super.onConfigurationChanged(newConfig);
	    setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
	}
	
	void unpauseEmulation() {
		if (DeSmuME.romLoaded)
			DeSmuME.unpauseEmulation();
	}
	
	void pauseEmulation() {
		if (DeSmuME.romLoaded)
			DeSmuME.pauseEmulation();
	}
	
	void pickRom() {
		Intent i = new Intent(this, FileDialog.class);
		i.setAction(Intent.ACTION_PICK);
		i.putExtra(FileDialog.START_PATH, Environment.getExternalStorageDirectory().getAbsolutePath());
		i.putExtra(FileDialog.FORMAT_FILTER, new String[] {".nds", ".zip", ".7z", ".rar"});
		startActivityForResult(i, PICK_ROM);
	}
	
	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		if(requestCode != PICK_ROM || resultCode != Activity.RESULT_OK)
			return;
		
		String romPath = data.getStringExtra(FileDialog.RESULT_PATH);
		if(romPath != null) {
			if(DeSmuME.romLoaded)
				DeSmuME.closeRom();
			
			final String loadingMsg = getResources().getString(R.string.loading);
			ProgressDialog loadingDialog = ProgressDialog.show(DeSmuMEActivity.this, null, loadingMsg, true);
			
			if(!DeSmuME.loadRom(romPath)) {
				DeSmuME.romLoaded = false;
				loadingDialog.dismiss();
				
				AlertDialog.Builder builder = new AlertDialog.Builder(DeSmuMEActivity.this);
				builder.setMessage(R.string.rom_error).setNegativeButton(R.string.OK, new DialogInterface.OnClickListener() {
					
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
			else {
				DeSmuME.romLoaded = true;
				loadingDialog.dismiss();
			}
		}
	}
	
	@Override
	public void onResume() {
		super.onResume();
		unpauseEmulation();
	}
	
	@Override
	public void onPause() {
		super.onPause();
		pauseEmulation();
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
		return true;
	}
	
	@Override
	public void onOptionsMenuClosed(Menu menu) {
		unpauseEmulation();
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
			restoreState(Integer.valueOf(item.getTitle().toString()));
			break;
		case R.id.save1: case R.id.save2: case R.id.save3: case R.id.save4: case R.id.save5:
		case R.id.save6: case R.id.save7: case R.id.save8: case R.id.save9:
			saveState(Integer.valueOf(item.getTitle().toString()));
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
		default:
			return false;
		}
		unpauseEmulation();
		return true;
	}
	
	void restoreState(int slot) {
		if(DeSmuME.romLoaded) {
			DeSmuME.restoreState(slot);
		}
	}
	
	void saveState(int slot) {
		if(DeSmuME.romLoaded) {
			DeSmuME.saveState(slot);
		}
	}
	
	@Override
	public void onSharedPreferenceChanged(SharedPreferences sharedPreferences, String key) {
		if(key != null) {
			if(DeSmuME.inited && key.equals(Settings.LANGUAGE))
				DeSmuME.reloadFirmware();
			
			DeSmuME.showfps = prefs.getBoolean(Settings.SHOW_FPS, false);
		}
		
		if(view != null) {
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
					
					synchronized(view) {
						view.resize(view.width, view.height, view.pixelFormat);
					}
				}
				else if(key.equals(Settings.RENDERER)) {
					int new3D = DeSmuME.getSettingInt(Settings.RENDERER, 2);
					DeSmuME.change3D(new3D);
				}
				else if(key.equals(Settings.CPUMODE)) {
					int newCpu = DeSmuME.getSettingInt(Settings.CPUMODE, 0);
					DeSmuME.changeCpuMode(newCpu);
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
			}
		}
	}
	
	class NDSView extends SurfaceView implements Callback {

		Bitmap emuBitmap;
		
		final Paint emuPaint = new Paint();
		final Paint hudPaint = new Paint();
		final float defhudsize = 15;
		float curhudsize = defhudsize;
		
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
		
		@Override
		public void onDraw(Canvas canvas) {
//			super.onDraw(canvas);
			
			canvas.drawColor(Color.BLACK);
			
			synchronized(view) {
				if(!DeSmuME.inited)
					return;
				
				if(emuBitmap == null)
					return;
				
				int data = DeSmuME.draw(emuBitmap);
				
				canvas.drawBitmap(view.emuBitmap, view.srcMain, view.destMain, emuPaint);
				canvas.drawBitmap(view.emuBitmap, view.srcTouch, view.destTouch, emuPaint);
				
				controls.drawControls(canvas);
				
				if (DeSmuME.showfps)
				{
					int fps = (data >> 24) & 0xFF;
					int fps3d = (data >> 16) & 0xFF;
					int cpuload0 = (data >> 8) & 0xFF;
					int cpuload1 = data & 0xFF;
					
					String hud = "Fps:"+fps+"/"+fps3d+"("+cpuload0+"%/"+cpuload1+"%)";
						
					canvas.drawText(hud, 10, curhudsize, hudPaint);
				}
			}
			
			invalidate();
		}
		
		@Override
		public boolean onTouchEvent(MotionEvent event) {
			return controls.onTouchEvent(event);
		}
		
		@Override
		public boolean onKeyDown(int keyCode, KeyEvent event) {
			return controls.onKeyDown(keyCode, event);
		}
		
		@Override
		public boolean onKeyUp(int keyCode, KeyEvent event) {
			return controls.onKeyUp(keyCode, event);
		}
		
		boolean doForceResize = false;
		public void forceResize() {
			doForceResize = true;
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
			sourceWidth = DeSmuME.getNativeWidth();
			sourceHeight = DeSmuME.getNativeHeight();
			resized = true;
			
			final boolean hasScreenFilter = DeSmuME.getSettingInt(Settings.SCREEN_FILTER, 0) != 0;
			final boolean is565 = newPixelFormat == PixelFormat.RGB_565 && !hasScreenFilter;
			landscape = newWidth > newHeight;
			controls.setView(this);
			controls.loadControls(DeSmuMEActivity.this, newWidth, newHeight, is565, landscape);
			
			forceTouchScreen = !prefs.getBoolean("Controls." + (landscape ? "Landscape." : "Portrait.") + "Draw", false);
			
			if(landscape) {
				destMain = new Rect(0, 0, newWidth / 2, newHeight);
				destTouch = new Rect(newWidth / 2, 0, newWidth, newHeight);
			}
			else {
				destMain = new Rect(0, 0, newWidth, newHeight / 2);
				destTouch = new Rect(0, newHeight / 2, newWidth, newHeight);
			}
			
			srcMain = new Rect(0, 0, sourceWidth, sourceHeight / 2);
			srcTouch = new Rect(0, sourceHeight / 2, sourceWidth, sourceHeight);
			
			emuBitmap = Bitmap.createBitmap(sourceWidth, sourceHeight, is565 ? Config.RGB_565 : Config.ARGB_8888);
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

		@Override
		public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
			synchronized(view) {
				resize(width, height, format);
			}
		}

		@Override
		public void surfaceCreated(SurfaceHolder holder) {
			
		}

		@Override
		public void surfaceDestroyed(SurfaceHolder holder) {
			
		}
	}
	
}
