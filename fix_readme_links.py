from pathlib import Path  
  
REPLACEMENTS = {  
    "firmware/esp8266/receiver/sketch_empfaengerESP.ino":  
        "firmware/esp8266/receiver/sketch_receiver.ino",  
  
    "firmware/esp8266/sender/sketch_senderESP.ino":  
        "firmware/esp8266/sender/sketch_sender.ino",  
  
    "hardware/pcb/receiver/rückseiteEmpfaenger.png":  
        "hardware/pcb/receiver/pcb_receiver_back.png",  
  
    "hardware/pcb/receiver/schaltplanEmpfaenger.png":  
        "hardware/pcb/receiver/schematic_receiver.png",  
  
    "hardware/pcb/sender/rückseite_r3_senderEsp.png":  
        "hardware/pcb/sender/pcb_sender_r3_back.png",  
  
    "hardware/pcb/sender/schaltplan_r3_senderEsp.png":  
        "hardware/pcb/sender/schematic_sender_r3.png",  
  
    "mechanics/3d_prints/receiver/EmpfaengerGehaeuse.stl":  
        "mechanics/prints_3d/receiver/receiver_housing.stl",  
  
    "mechanics/3d_prints/receiver/EmpfaengerGehaeusePCB.jpg":  
        "mechanics/prints_3d/receiver/receiver_housing.png",  
  
    "mechanics/3d_prints/reed_sensor/reed_sensor_gehaeuse.stl":  
        "mechanics/prints_3d/reed_sensor/reed_sensor_gehaeuse.stl",  
  
    "mechanics/3d_prints/rfid_sensor/rfid_sensor_gehaeuse.stl":  
        "mechanics/prints_3d/rfid_sensor/rfid_sensor_gehaeuse.stl",  
  
    "media/photos/gehauesePrototyp.png":  
        "media/photos/prototype_housing.png",  
  
    "media/videos/gifs/sender_pcb.gif":  
        "media/videos/gifs/sender_r3_pcb.gif",  
  
    "photos/prototyp_breadboards.png":  
        "media/photos/prototype_breadboards.png",  
  
    "photos/prototyp_perforatedCircuitBoards.jpg":  
        "media/photos/prototype_perforated_circuit_boards.jpg",  
}  
  
  
def fix_readme(readme: Path):  
    text = readme.read_text(encoding="utf-8")  
    original = text  
    changed = False  
  
    for old, new in REPLACEMENTS.items():  
        if old in text:  
            print(f"✔ {readme}: {old} -> {new}")  
            text = text.replace(old, new)  
            changed = True  
  
    if changed:  
        readme.write_text(text, encoding="utf-8")  
        print(f"✅ updated {readme}")  
    else:  
        print(f"ℹ no changes in {readme}")  
  
  
def main():  
    readmes = list(Path(".").rglob("README*.md"))  
  
    if not readmes:  
        print("❌ no README files found")  
        return  
  
    print(f"🔍 found {len(readmes)} README files\n")  
  
    for readme in readmes:  
        fix_readme(readme)  
  
  
if __name__ == "__main__":  
    main()  
  
