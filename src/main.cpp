extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
#ifndef DEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= Version::PROJECT;
	*path += ".log"sv;
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef DEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_skse->IsEditor()) {
		logger::critical("Loaded in editor, marking as incompatible"sv);
		return false;
	}

	const auto ver = a_skse->RuntimeVersion();
	if (ver < SKSE::RUNTIME_1_5_39) {
		logger::critical(FMT_STRING("Unsupported runtime version {}"), ver.string());
		return false;
	}

	return true;
}


class AmmoHook
{
public:
	static void Hook()
	{
		ammo = RE::TESForm::LookupByID<RE::TESAmmo>(0x000139BE);
		REL::safe_write(REL::ID(38046).address() + 0x451, "\xeb\x34", 2);
		_GetCurrentAmmo = REL::Relocation<uintptr_t>(RE::VTABLE_PlayerCharacter[0]).write_vfunc(0x9E, GetCurrentAmmo);
		_AttachArrow = SKSE::GetTrampoline().write_call<5>(REL::ID(40250).address() + 0x75, AttachArrow);
		_AttachArrow = SKSE::GetTrampoline().write_branch<5>(REL::ID(40250).address() + 0x9d, AttachArrow);
	}

private:
	static RE::TESAmmo* GetCurrentAmmo(RE::Actor* a)
	{
		auto ans = _GetCurrentAmmo(a);
		return ans ? ans : ammo;
	}

	static void AttachArrow(RE::Actor* a, const RE::BSTSmartPointer<RE::BipedAnim>& biped)
	{
		if (a->IsPlayerRef()) {
			auto proc = a->currentProcess;
			if (!_generic_foo_<38782, RE::InventoryEntryData*(RE::AIProcess*)>::eval(proc) &&
				!ammo->data.flags.any(RE::AMMO_DATA::Flag::kNonPlayable)) {
				if (auto quiver =
						_generic_foo_<38801, RE::NiNode*(RE::AIProcess*, const RE::BSTSmartPointer<RE::BipedAnim>& biped)>::eval(
							proc, biped)) {
					auto _weap_node = ammo->data.flags.any(RE::AMMO_DATA::Flag::kNonBolt) ? proc->GetWeaponNode(biped) :
					                                                                        proc->GetMagicNode(biped);
					auto weap_node = _weap_node->AsNode();
					auto quiver_arrow0 = quiver->GetObjectByName(RE::FixedStrings::GetSingleton()->arrow0);
					RE::TaskQueueInterface::GetSingleton()->QueueNodeAttach(quiver_arrow0->Clone(), weap_node);
					return;
				}
			}
		}

		return _AttachArrow(a, biped);
	}

	static inline RE::TESAmmo* ammo;
	static inline REL::Relocation<decltype(GetCurrentAmmo)> _GetCurrentAmmo;
	static inline REL::Relocation<decltype(AttachArrow)> _AttachArrow;
};


float add_rot_x(float val, float d)
{
	const float PI = 3.1415926f;
	// -pi/2..pi/2
	d = d * PI / 180.0f;
	val += d;
	val = std::max(val, -PI / 2);
	val = std::min(val, PI / 2);
	return val;
}

float add_rot_z(float val, float d)
{
	const float PI = 3.1415926f;
	// -pi/2..pi/2
	d = d * PI / 180.0f;
	val += d;
	while (val < 0) val += 2 * PI;
	while (val > 2 * PI) val -= 2 * PI;
	return val;
}

auto add_rot(RE::Projectile::ProjectileRot rot, RE::Projectile::ProjectileRot delta)
{
	rot.x = add_rot_x(rot.x, delta.x);
	rot.z = add_rot_z(rot.z, delta.z);
	return rot;
}

auto add_rot_rnd(RE::Projectile::ProjectileRot rot, RE::Projectile::ProjectileRot rnd)
{
	if (rnd.x == 0 && rnd.z == 0)
		return rot;

	rot.x = add_rot_x(rot.x, rnd.x * FenixUtils::random_range(-1.0f, 1.0f));
	rot.z = add_rot_z(rot.z, rnd.z * FenixUtils::random_range(-1.0f, 1.0f));
	return rot;
}

class DataHandler
{
public:
	static inline std::map<RE::FormID, std::pair<uint32_t, uint32_t>> data;

	static void init()
	{
		data.clear();

		FILE* fp;

		fopen_s(&fp, "Data/skse/plugins/CustomAmmo/CustomAmmo.txt", "r");
		if (fp) {
			// Skyrim.esm|0x31415 7 5
			char mod_name[50] = { 0 };
			uint32_t formid, projs, recoil;
			while (fscanf_s(fp, "%s 0x%x %u %u", mod_name, 50, &formid, &projs, &recoil) != EOF) {
				if (auto weap = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESObjectWEAP>(formid, mod_name)) {
					if (data.find(weap->formID) == data.end()) {
						if (projs > 0) {
							if (recoil >= 0 && recoil <= 90)
								data.insert({ weap->formID, { projs, recoil } });
							else
								logger::warn("weap 0x{:x} has wrong recoil {}", weap->formID, recoil);
						} else {
							logger::warn("weap 0x{:x} has wrong number of projs {}", weap->formID, projs);
						}
					} else {
						logger::warn("weap 0x{:x} already in the map", weap->formID);
					}
				} else {
					logger::warn("weap 0x{:x} is not a weap or does not exists in plugin {}", formid, mod_name);
				}
			}
			fclose(fp);
		} else {
			logger::warn("File not found");
		}
	}
};

class InputHandler : public RE::BSTEventSink<RE::InputEvent*>
{
public:
	static InputHandler* GetSingleton()
	{
		static InputHandler singleton;
		return std::addressof(singleton);
	}

	RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* e, RE::BSTEventSource<RE::InputEvent*>*) override
	{
		if (!*e)
			return RE::BSEventNotifyControl::kContinue;

		if (auto buttonEvent = (*e)->AsButtonEvent();
			buttonEvent && buttonEvent->HasIDCode() && (buttonEvent->IsDown() || buttonEvent->IsPressed())) {
			if (int key = buttonEvent->GetIDCode(); key == 71) {
				DataHandler::init();
			}

			// Attach quiver
			//if (int key = buttonEvent->GetIDCode(); key == 72) {
			//	_generic_foo_<19346, void(RE::Actor*, RE::TESAmmo*)>::eval(RE::PlayerCharacter::GetSingleton(),
			//		RE::TESForm::LookupByID<RE::TESAmmo>(0x000139BE));
			//}
		}
		return RE::BSEventNotifyControl::kContinue;
	}

	void enable()
	{
		if (auto input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink(this);
		}
	}
};

class AutoEquipHook
{
public:
	static void Hook()
	{
		_AutoEquip1 = SKSE::GetTrampoline().write_call<5>(REL::ID(15827).address() + 0xcdf, AutoEquip1);
		_AutoEquip2 = SKSE::GetTrampoline().write_call<5>(REL::ID(39456).address() + 0x5bc, AutoEquip2);
		_AutoEquip3 = SKSE::GetTrampoline().write_call<5>(REL::ID(40240).address() + 0x51, AutoEquip3);

		_Launch = SKSE::GetTrampoline().write_call<5>(REL::ID(33672).address() + 0x377, Launch);
	}

private:
	static bool should_autoequip(RE::Actor* a) {
		auto weap = _generic_foo_<37621, RE::TESObjectWEAP*(RE::Actor * a, bool left)>::eval(a, false);
		return !weap->HasKeywordString("Gun_FireEffect");
	}

	static RE::ProjectileHandle* Launch(RE::ProjectileHandle* handle, RE::Projectile::LaunchData& ldata)
	{
		if (auto a = ldata.shooter->As<RE::Actor>();
			a && (ldata.castingSource == RE::MagicSystem::CastingSource::kLeftHand ||
					 ldata.castingSource == RE::MagicSystem::CastingSource::kRightHand)) {
			if (auto item = a->GetEquippedEntryData(ldata.castingSource == RE::MagicSystem::CastingSource::kLeftHand);
				item && item->object && item->object->As<RE::TESObjectWEAP>()) {
				auto weap = item->object->As<RE::TESObjectWEAP>();
				if (auto found = DataHandler::data.find(weap->formID); found != DataHandler::data.end()) {
					auto projs = (*found).second.first;
					float recoil = static_cast<float>((*found).second.second);

					RE::Projectile::ProjectileRot origin = { ldata.angleX, ldata.angleZ };
					RE::Projectile::ProjectileRot delta = { recoil, recoil };

					for (size_t i = 0; i < projs - 1; i++) {
						auto new_rot = add_rot_rnd(origin, delta);
						ldata.angleX = new_rot.x;
						ldata.angleZ = new_rot.z;
						RE::Projectile::Launch(handle, ldata);
						if (auto proj = handle->get().get()) {
							proj->flags.set(RE::Projectile::Flags::kUseOrigin);
							proj->flags.reset(RE::Projectile::Flags::kAutoAim);
						}
					}

					auto new_rot = add_rot_rnd(origin, delta);
					ldata.angleX = new_rot.x;
					ldata.angleZ = new_rot.z;
				}
			}
		}

		return _Launch(handle, ldata);
	}

	static void AutoEquip1(RE::Actor* a)
	{
		if (should_autoequip(a))
			return _AutoEquip1(a);
	}
	static void AutoEquip2(RE::Actor* a)
	{
		if (should_autoequip(a))
			return _AutoEquip2(a);
	}
	static void AutoEquip3(RE::Actor* a)
	{
		if (should_autoequip(a))
			return _AutoEquip3(a);
	}

	static inline REL::Relocation<decltype(Launch)> _Launch;
	static inline REL::Relocation<decltype(AutoEquip1)> _AutoEquip1;
	static inline REL::Relocation<decltype(AutoEquip2)> _AutoEquip2;
	static inline REL::Relocation<decltype(AutoEquip3)> _AutoEquip3;
};

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
{
	switch (message->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		//AmmoHook::Hook();
		InputHandler::GetSingleton()->enable();
		AutoEquipHook::Hook();
		DataHandler::init();

		break;
	}
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	auto g_messaging = reinterpret_cast<SKSE::MessagingInterface*>(a_skse->QueryInterface(SKSE::LoadInterface::kMessaging));
	if (!g_messaging) {
		logger::critical("Failed to load messaging interface! This error is fatal, plugin will not load.");
		return false;
	}

	logger::info("loaded");

	SKSE::Init(a_skse);
	SKSE::AllocTrampoline(1 << 10);

	g_messaging->RegisterListener("SKSE", SKSEMessageHandler);

	return true;
}
