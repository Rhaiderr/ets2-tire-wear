# Where the textures of each catalog source were extracted (probe\, not in the
# repository): returns a hashtable source label -> folder for ets2 or ats.
param([string]$Project, [string]$Game)
if ($Game -eq 'ats') {
    @{
        'base_vehicle' = "$Project\probe\ats\extracted_base_vehicle"
        'dlc_goodyear' = "$Project\probe\ats\extracted_dlc_goodyear"
        'dlc_michelin' = "$Project\probe\ats\extracted_dlc_michelin"
        'dlc_rims'     = "$Project\probe\ats\extracted_dlc_rims"
    }
} else {
    @{
        'base_vehicle' = "$Project\probe\extracted"
        'dlc_goodyear' = "$Project\probe\extracted_dlc_goodyear"
        'dlc_michelin' = "$Project\probe\extracted_dlc_michelin"
    }
}
