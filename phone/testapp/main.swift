// SweetHello: smallest UIKit app to test installing third-party apps on the port.
// Shows its data container path and a launch counter kept in UserDefaults (needs a working container).
import UIKit

final class ViewController: UIViewController {
	override func viewDidLoad() {
		super.viewDidLoad()
		view.backgroundColor = .systemIndigo
		let d = UserDefaults.standard
		let n = d.integer(forKey: "launches") + 1
		d.set(n, forKey: "launches")
		let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
		let f = docs.appendingPathComponent("hello.txt")
		let wrote = (try? "launch \(n)\n".write(to: f, atomically: true, encoding: .utf8)) != nil
		let l = UILabel()
		l.numberOfLines = 0
		l.textAlignment = .center
		l.textColor = .white
		l.font = .systemFont(ofSize: 20, weight: .semibold)
		l.text = "Hello from a sideloaded app\n\nlaunch \(n)\nfile write \(wrote ? "ok" : "FAILED")\n\n\(NSHomeDirectory())"
		l.translatesAutoresizingMaskIntoConstraints = false
		view.addSubview(l)
		NSLayoutConstraint.activate([
			l.centerXAnchor.constraint(equalTo: view.centerXAnchor),
			l.centerYAnchor.constraint(equalTo: view.centerYAnchor),
			l.leadingAnchor.constraint(greaterThanOrEqualTo: view.leadingAnchor, constant: 20),
		])
		NSLog("SweetHello: launch %d home %@ write %d", n, NSHomeDirectory(), wrote ? 1 : 0)
	}
}

final class AppDelegate: UIResponder, UIApplicationDelegate {
	var window: UIWindow?
	func application(_ application: UIApplication,
	                 didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
		window = UIWindow(frame: UIScreen.main.bounds)
		window!.rootViewController = ViewController()
		window!.makeKeyAndVisible()
		return true
	}
}

UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
