<div dir="rtl">

# PwfAuth-TimeSync

ضبط وقت الويندوز تلقائياً قبل تفعيل الكي في برامج [pwfauth](https://pwfauth.com). يعني نفس زر **Sync now** اللي في إعدادات الويندوز، بس البرنامج يضغطه عن العميل قبل لا يرسل الكي.

يشتغل على **Windows 10** و **Windows 11**، وفيه نسخة جاهزة لـ **C#** و **C++** و **Python** (نفس اللغات اللي لها SDK رسمي في pwfauth).

---

## المشكلة

أي برنامج يستخدم pwfauth يرسل مع كل طلب الوقت الحالي من جهاز العميل، والسيرفر يرفض الطلب إذا كان وقت الجهاز مختلف عن وقت السيرفر بأكثر من **5 دقائق**.

النتيجة: العميل يحط الكي وهو صحيح، ويطلع له إن الكي مرفوض أو `CLOCK_SKEW`. وبعدها لازم يروح بنفسه:

`الإعدادات > الوقت واللغة > التاريخ والوقت > Sync now`

وأغلب العملاء ما يعرفون هالشي، فيحسبون الكي خربان أو البرنامج فيه مشكلة ويجونك في الخاص.

## الحل

تضيف ملف واحد لمشروعك وتستدعي دالة وحدة **قبل** تسجيل الدخول بالكي. الكود يسوي التالي:

1. يقارن وقت الجهاز بوقت سيرفرات NTP الرسمية (time.windows.com و time.google.com و time.cloudflare.com و pool.ntp.org) ويسألهم كلهم مع بعض وياخذ أسرع رد. وإذا كان منفذ NTP مقفل عند العميل، ياخذ الوقت من هيدر `Date` في سيرفرات HTTP.
2. إذا كان الوقت مضبوط (الفرق أقل من 30 ثانية) ما يسوي شي ويكمل على طول. هذي الحالة عند أغلب العملاء وتاخذ أقل من ثانية.
3. إذا كان الوقت غلط يشغّل نفس أمر **Sync now** اللي يشغله الويندوز من الإعدادات (`SystemSettingsAdminFlows.exe ForceTimeSync 1`)، وهذا يشتغل بدون أي نافذة صلاحيات عند أغلب المستخدمين.
4. إذا البرنامج شغال كمسؤول (Run as administrator) ولسا الوقت ما انضبط، يشغّل خدمة الوقت ويسوي `w32tm /resync`، وإذا ما نفع يضبط الساعة مباشرة من وقت NTP.
5. وإذا ما نفع شي من اللي فوق، يعيد تشغيل نفس البرنامج كمسؤول لثواني بس عشان يضبط الوقت ويقفل (تطلع للعميل رسالة UAC وحدة). وتقدر تطفي هالخطوة من الإعدادات.

بعدها البرنامج يكمل تسجيل الدخول بالكي عادي والوقت مضبوط.

## وش الفايدة

- **عند بعض العملاء المشكلة ما راح تظهر أصلاً.** الكود يصلح الوقت قبل لا يوصل الكي للسيرفر، فما تطلع رسالة رفض الكي من الأساس.
- **تجربة أسلس لعملائك.** ما يحتاج العميل يفتح إعدادات ولا يدور على Sync now ولا يراسلك عشان يسأل ليش الكي ما اشتغل.
- **دعم فني أقل.** مشكلة "حطيت الكي وما ضبط" بسبب الوقت تختفي.
- **آمن.** ما يغير إعدادات خدمة الوقت في الويندوز، ولا يغير المنطقة الزمنية، ولا يلمس شي إذا الوقت أصلاً مضبوط.
- **ما يوقف برنامجك.** الدالة ما ترمي أي Exception، وإذا ما قدرت توصل لأي سيرفر وقت ترجع وتخلي البرنامج يكمل عادي.

---

## طريقة الإضافة

### C# (WinForms / WPF / Console)

1. انسخ الملف [`csharp/AutoTimeSync.cs`](csharp/AutoTimeSync.cs) لمشروعك.
2. أول سطر في `Main` حط `HandleElevatedCommand`، وقبل `LoginAsync` حط `EnsureClockIsCorrect`:

</div>

```csharp
using PwfTimeSync;

static async Task Main(string[] args)
{
    // لازم يكون أول سطر في البرنامج
    if (AutoTimeSync.HandleElevatedCommand(args)) return;

    // يضبط وقت الويندوز قبل تسجيل الدخول بالكي
    AutoTimeSync.EnsureClockIsCorrect();

    var client = new PwfClient("APP_SECRET");
    var login = await client.LoginAsync(licenseKey);
    if (!login.Success)
    {
        Console.WriteLine(login.Message);
        return;
    }

    client.StartHeartbeat();
}
```

<div dir="rtl">

في WinForms نفس الشي داخل `Program.cs`:

</div>

```csharp
[STAThread]
static void Main(string[] args)
{
    if (AutoTimeSync.HandleElevatedCommand(args)) return;
    AutoTimeSync.EnsureClockIsCorrect();

    Application.EnableVisualStyles();
    Application.Run(new LoginForm());
}
```

<div dir="rtl">

وإذا تبي تستدعيه من داخل فورم بدون ما يعلق الواجهة استخدم النسخة الـ async:

</div>

```csharp
await AutoTimeSync.EnsureClockIsCorrectAsync();
var login = await client.LoginAsync(txtKey.Text);
```

<div dir="rtl">

### C++

1. انسخ الملف [`cpp/AutoTimeSync.h`](cpp/AutoTimeSync.h) لمشروعك (ملف واحد بس، ما يحتاج `.cpp`).
2. استدعه قبل `client.Login`:

</div>

```cpp
#include "AutoTimeSync.h"
#include "PwfClient.h"

int main()
{
    // لازم يكون أول شي في البرنامج
    if (PwfTimeSync::HandleElevatedCommand()) return 0;

    // يضبط وقت الويندوز قبل تسجيل الدخول بالكي
    PwfTimeSync::EnsureClockIsCorrect();

    PwfClient client(appSecret);
    auto login = client.Login(licenseKey);
    // ...
}
```

<div dir="rtl">

ملاحظات:

- حط `#include "AutoTimeSync.h"` **قبل** `PwfClient.h` وقبل `<windows.h>` عشان ما يصير تعارض بين `winsock2.h` و `winsock.h`.
- يحتاج C++17، والمكتبات (`ws2_32` و `advapi32` و `shell32` و `user32`) تنربط تلقائي في Visual Studio. في MinGW ضيفها يدوي:
`-lws2_32 -ladvapi32 -lshell32 -luser32`
- في برامج الواجهة (GUI) مرر نافذتك الرئيسية في `options.Owner` عشان لو طلعت رسالة صلاحيات تطلع قدام البرنامج مو وراه.

### Python (حتى لو محوّل لـ exe بـ PyInstaller)

1. انسخ الملف [`python/auto_timesync.py`](python/auto_timesync.py) جنب ملف برنامجك. ما يحتاج أي مكتبة خارجية.
2. استدعه قبل `client.login`:

</div>

```python
import auto_timesync
from pwfauth import PwfClient

if __name__ == "__main__":
    # لازم يكون أول شي في البرنامج
    auto_timesync.handle_elevated_command()

    # يضبط وقت الويندوز قبل تسجيل الدخول بالكي
    auto_timesync.ensure_clock_is_correct()

    client = PwfClient("APP_SECRET")
    login = client.login(license_key)
```

<div dir="rtl">

---

## ليش `HandleElevatedCommand` لازم يكون أول سطر؟

في الحالات النادرة اللي ما ينفع فيها Sync now العادي، الكود يعيد تشغيل **نفس البرنامج** كمسؤول ومعه باراميتر خاص (`--pwf-timesync-elevated`). هالسطر هو اللي يلقط هالباراميتر، فيضبط الوقت ويقفل النسخة هذي على طول بدون ما تفتح واجهة البرنامج ولا تطلب كي.

إذا ما حطيته في أول `Main`، النسخة اللي انفتحت كمسؤول راح تفتح برنامجك كامل مرة ثانية.

## الإعدادات

كل الإعدادات اختيارية، والقيم الافتراضية تناسب pwfauth. الأسماء نفسها في C# و C++، وفي Python نفس الأسماء بس بصيغة `snake_case` (مثلاً `allow_elevation_prompt`):

| الإعداد | الافتراضي | وش يسوي |
|---|---|---|
| `MaxAllowedOffsetSeconds` | `30` | أقصى فرق مسموح بالثواني قبل ما يضبط الوقت (pwfauth يرفض بعد 300) |
| `AllowElevationPrompt` | `true` | يسمح يطلع رسالة UAC كآخر حل. خله `false` إذا ما تبي أي نافذة صلاحيات |
| `ForceSync` | `false` | يسوي Sync now حتى لو الوقت مضبوط (مفيد للتجربة) |
| `SyncNowTimeoutMs` | `15000` | أقصى وقت ينتظر فيه Sync now |
| `NtpServers` | time.windows.com, time.google.com, time.cloudflare.com, pool.ntp.org | سيرفرات الوقت |
| `Log` | بدون | دالة تستقبل رسائل السجل لو تبي تشوف وش صار |

مثال C#:

</div>

```csharp
var result = AutoTimeSync.EnsureClockIsCorrect(new TimeSyncOptions
{
    AllowElevationPrompt = false,
    Log = msg => File.AppendAllText("timesync.log", msg + Environment.NewLine)
});

if (!result.Success)
    MessageBox.Show("وقت جهازك غير مضبوط، فعّل ضبط الوقت تلقائياً من إعدادات الويندوز ثم حاول مرة ثانية.");
```

<div dir="rtl">

النتيجة (`TimeSyncResult`) فيها:

- `Success`: هل الوقت صار مضبوط
- `Method`: وش الطريقة اللي انضبط فيها (`AlreadyCorrect` أو `WindowsSyncNow` أو `W32tmResync` أو `SetSystemTime` أو `ElevatedHelper` أو `NoReference` أو `Failed`)
- `OffsetBeforeSeconds` و `OffsetAfterSeconds`: الفرق بالثواني قبل وبعد

## تجربة سريعة

</div>

```bash
cd csharp/examples/ConsoleExample
dotnet run -f net8.0
```

<div dir="rtl">

ولو تبي تجرب زر Sync now نفسه حتى لو وقتك مضبوط:

</div>

```bash
dotnet run -f net8.0 -- --force
```

<div dir="rtl">

## أسئلة شائعة

**هل يحتاج البرنامج يشتغل كمسؤول؟**
لا. أغلب الحالات تنحل بـ Sync now العادي بدون صلاحيات. خطوة المسؤول تجي بس إذا كل شي ثاني فشل.

**إذا كانت المنطقة الزمنية غلط؟**
pwfauth يعتمد على توقيت UTC، فالمنطقة الزمنية الغلط ما تسبب رفض الكي، والكود ما يلمسها.

**هل يبطئ فتح البرنامج؟**
لا. إذا الوقت مضبوط ياخذ أقل من ثانية في العادة، وفي الشبكات اللي تقفل منفذ NTP يزيد تقريباً ثانية ونص لأنه يروح لطريقة HTTP.

**إذا جهاز العميل بدون نت؟**
يرجع `NoReference` ويخلي برنامجك يكمل عادي، وأصلاً تسجيل الدخول بالكي يحتاج نت.

**هل يشتغل على ويندوز 10؟**
إيه، على ويندوز 10 و 11. وفي نسخ ويندوز 10 القديمة اللي ما فيها Sync now، يروح للطرق الثانية تلقائي.

---

## License

MIT © [Fahads7ee7](https://github.com/Fahads7ee7)

</div>
