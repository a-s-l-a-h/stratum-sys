#include <stratum/stratum.hpp>
#include <stratum/android/widget/LinearLayout.hpp>
#include <stratum/android/widget/TextView.hpp>
#include <stratum/android/widget/Button.hpp>
#include <stratum/android/widget/Toast.hpp>

using stratum::Activity;
using stratum::JStr;
using stratum::android::widget::LinearLayout;
using stratum::android::widget::TextView;
using stratum::android::widget::Button;
using stratum::android::widget::Toast;

// Called automatically once the Activity is ready (weak-symbol auto-wiring).
void onCreate(Activity act) {
    LinearLayout root(act);
    root.setOrientation(1);                       // VERTICAL
    root.setPadding(48, 48, 48, 48);

    TextView title(act);
    title.setText(JStr("Hello from Stratum C++"));
    title.setTextSize(22.0f);
    root.addView(title);

    Button btn(act);
    btn.setText(JStr("Tap me"));
    btn.setOnClickListener([act]() {
        Toast::makeText(act, JStr("Clicked from C++"), 0).unchecked_as<Toast>().show();
    });
    root.addView(btn);

    stratum::setContentView(act, root);
}