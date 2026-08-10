package org.meumeu.wivrn;

public class RestartActivity extends android.app.Activity
{
	public static final String EXTRA_RESTART_INTENT = "org.meumeu.wivrn.RESTART_INTENT";

	@SuppressWarnings("deprecation")
	private android.content.Intent getRestartIntent()
	{
		if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU)
			return this.getIntent().getParcelableExtra(EXTRA_RESTART_INTENT, android.content.Intent.class);
		return this.getIntent().getParcelableExtra(EXTRA_RESTART_INTENT);
	}

	@Override
	protected void onCreate(android.os.Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);

		final android.content.Intent restartIntent = this.getRestartIntent();
		if (restartIntent == null)
		{
			this.finishAndRemoveTask();
			return;
		}

		final android.os.Handler handler = new android.os.Handler(android.os.Looper.getMainLooper());
		handler.postDelayed(() -> {
			this.startActivity(restartIntent);
			this.finishAndRemoveTask();
			handler.postDelayed(
			        () -> android.os.Process.killProcess(android.os.Process.myPid()), 500);
		}, 1500);
	}
}
