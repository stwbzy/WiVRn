package org.meumeu.wivrn;

public class MainActivity extends android.app.NativeActivity
{
	static
	{
		System.loadLibrary("wivrn");
	}

	@Override
	public native void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults);

	@Override
	public native void onNewIntent(android.content.Intent intent);

	@Override
	public native void onActivityResult(int requestCode, int resultCode, android.content.Intent data);

	public void restartAfterRuntimeFailure()
	{
		final android.content.Intent restartIntent = new android.content.Intent(this.getIntent());
		restartIntent.setClass(this, MainActivity.class);
		restartIntent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK |
		                       android.content.Intent.FLAG_ACTIVITY_CLEAR_TASK);

		final android.content.Intent trampolineIntent = new android.content.Intent(this, RestartActivity.class);
		trampolineIntent.putExtra(RestartActivity.EXTRA_RESTART_INTENT, restartIntent);
		this.startActivity(trampolineIntent);
	}

	private android.content.BroadcastReceiver BatteryInfoReceiver = null;

	@Override
	protected void onCreate(android.os.Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);

		this.BatteryInfoReceiver = new BroadcastReceiver();

		this.registerReceiver(this.BatteryInfoReceiver, new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED));
	}
}
