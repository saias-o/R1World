#include "session.hpp"
#include "photo_detection.hpp"

namespace r1::world {
namespace {constexpr size_t kPortfolioPageSize=8;}
void World::Impl::loadPlayerProfile() {
    // Test runs never read or modify the real player's progress.
    const bool photoTest=smoke&&std::getenv("R1WORLD_PHOTO_SMOKE");
    profileStore=std::make_unique<PlayerProfileStore>(photoTest?game/"generated/photo-smoke":game/"saves");
    if(!testRun())profile=profileStore->load(profileError);
    if(!profileError.empty())saida::Log::error("[World profile] ",profileError);
    arsenal.restore(profile.arsenal);
}

bool World::Impl::savePlayerProfile() {
    if(!profileStore||(testRun()&&!(smoke&&std::getenv("R1WORLD_PHOTO_SMOKE"))))return true;
    profile.arsenal=arsenal.snapshot();
    std::string error;
    if(!profileStore->save(profile,error)) {
        if(profileError!=error)saida::Log::error("[World profile] save failed: ",error);
        profileError=error;profileDirty=true;
        photoNotice("Sauvegarde impossible : "+error);return false;
    }
    profileError.clear();profileDirty=false;return true;
}

void World::Impl::photoNotice(const std::string& message) {
    text("photo-notice",message);style("photo-notice","display",message.empty()?"none":"block");photoNoticeAge=5.;
}

void World::Impl::showPlayerMenu(bool show) {
    if(show&&(!playing||menu||drop.phase!=Drop::None||photoResult))return;
    if(show) {
        if(cheatOpen)closeCheat();setPhotoMode(false);setAiming(false);
        // Vehicle commands are latched by the physics behaviour between game
        // updates. Opening a menu must release the previous accelerator.
        if(driving()&&car)if(auto* driver=r1::VehicleRuntime::driver(*car))driver->command(0,1,0,true);
    }
    if(!show)ui->executeJS("closePortfolioViewer();");
    playerMenu=show;
    style("player-menu","display",show?"block":"none");
    style("hud","display",show?"none":"block");
    minimapUi->setEnabled(!show&&!menu);
    if(performanceUi)performanceUi->setEnabled(performanceDebug&&!show&&!menu);
    engine.window().setCursorCaptured(!show&&!menu);
    wasFire=clicked(GLFW_MOUSE_BUTTON_LEFT);
    if(show) {
        style("photo-notice","display","none");photoNoticeAge=0;
        savePlayerProfile();refreshPlayerMenu();
    }
}

bool World::Impl::setPhotoMode(bool show) {
    if(show&&(!playing||menu||playerMenu||cheatOpen||playerDown||driving()||sailing()||piloting()||swimming()||
              drop.phase!=Drop::None||jumpOffset>.01||vr||photoResult)) {
        photoNotice("Posez-vous à pied pour sortir l’appareil photo.");return false;
    }
    if(show&&(!profileStore->writable()||!profile.items.count("camera"))) {
        photoNotice(profileStore->writable()?"Vous ne possédez pas d’appareil photo.":"Sauvegarde illisible : photos désactivées pour protéger le portfolio.");return false;
    }
    photoMode=show;
    if(show){setAiming(false);cameraFrom.reset();}
    player->setVisible(!show&&!vr);
    style("hud","display",show?"none":"block");
    style("photo-viewfinder","display",show?"block":"none");
    minimapUi->setEnabled(!show&&!menu&&!playerMenu);
    camera->fovDegrees=sceneFov;
    wasPhotoShutter=clicked(GLFW_MOUSE_BUTTON_LEFT)||held(GLFW_KEY_ENTER);
    wasFire=clicked(GLFW_MOUSE_BUTTON_LEFT);
    text("photo-category",categoryLabel(photoCategory));
    text("photo-zoom","1.0×");
    return true;
}

void World::Impl::takePhotograph() {
    if(!photoMode||photoResult)return;
    try {
        if(smoke&&std::getenv("R1WORLD_PHOTO_ANIMAL_SMOKE")&&profile.photos.empty()) {
            if(!framePhotoSmokeAnimal())throw std::runtime_error("no visible animal available for the photo smoke");
        }
        auto result=std::make_shared<PhotoResult>();
        auto& photo=result->photo;photo=profileStore->newPhoto();
        const auto eye=sceneToGeo(camera->transform().position);
        photo.longitude=eye.x;photo.latitude=eye.y;photo.altitude=eye.z;
        photo.category=photoCategory;
        photo.fovDegrees=camera->fovDegrees;
        std::vector<const r1::MiniMapTile*> maps;
        for(const auto& [key,t]:loaded)maps.push_back(&t.served->cooked.minimap);
        const std::string zone=localConditions()?conditions.value("timezone",std::string()):"";
        photo.place=r1::miniMapPlace(maps,zoneCountries,countryNames,zone,lon,lat);
        const std::string missingCountry=" · Unknown country";
        if(photo.place.size()>=missingCountry.size()&&photo.place.compare(photo.place.size()-missingCountry.size(),missingCountry.size(),missingCountry)==0)
            photo.place.resize(photo.place.size()-missingCountry.size());
        if(photo.place=="Unknown country")photo.place.clear();
        photo.title=std::string(categoryLabel(photoCategory))+" · "+(photo.place.empty()?"Exploration":photo.place);
        if(photo.title.size()>512)photo.title.resize(512);
        classifyPhotograph(photo,photographedAnimals());
        const auto path=profileStore->photoPath(photo);
        fs::create_directories(path.parent_path());
        if(fs::exists(path))throw std::runtime_error("photo path already exists");
        std::string error;
        if(!engine.captureFramePng(path.string(),[result](bool ok,const std::string& error){
            result->success=ok;result->error=error;result->done=true;
        },error))throw std::runtime_error(error);
        photoResult=std::move(result);
        // The renderer captures the complete composite. Disable every game UI
        // for the exposure rather than baking the viewfinder into the image.
        ui->setEnabled(false);minimapUi->setEnabled(false);
        if(performanceUi)performanceUi->setEnabled(false);
        if(arrivalUi)arrivalUi->setEnabled(false);
    } catch(const std::exception& e) {
        saida::Log::error("[World photo] ",e.what());photoNotice("Photo impossible : "+std::string(e.what()));
    }
}

void World::Impl::finishPhotograph() {
    if(!photoResult||!photoResult->done)return;
    auto result=std::move(photoResult);
    ui->setEnabled(true);
    if(performanceUi)performanceUi->setEnabled(performanceDebug&&!playerMenu&&!menu&&!photoMode);
    if(!result->success) {
        saida::Log::error("[World photo] capture failed: ",result->error);
        photoNotice("Photo impossible : "+result->error);return;
    }
    profile.photos.push_back(std::move(result->photo));profileDirty=true;
    portfolioFilter=-1;portfolioSpecies.clear();portfolioIndex=profile.photos.size()-1;
    if(savePlayerProfile())photoNotice(profile.photos.back().animals.empty()?"Photo enregistrée · M pour ouvrir le portfolio":
        "Animal détecté : "+profile.photos.back().animals.front().name+" · photo classée dans le portfolio");
    else photoNotice("Image conservée sur disque ; sauvegarde du portfolio impossible. Réessayez avec M.");
    saida::Log::info("[World photo] wrote ",profileStore->photoPath(profile.photos.back()).string());
}

void World::Impl::updatePhotography(double dt) {
    finishPhotograph();
    if(photoNoticeAge>0){photoNoticeAge-=dt;if(photoNoticeAge<=0)style("photo-notice","display","none");}
    profileSaveAge+=dt;
    if(profileSaveAge>=2.) {
        profileSaveAge=0;
        if(profileDirty||profile.arsenal!=arsenal.snapshot())savePlayerProfile();
    }
    const bool m=held(GLFW_KEY_M),p=held(GLFW_KEY_P),tab=held(GLFW_KEY_TAB);
    const bool shutter=clicked(GLFW_MOUSE_BUTTON_LEFT)||held(GLFW_KEY_ENTER);
    if(!cheatOpen&&!optionsOpen&&!performanceDebug) {
        if(m&&!wasPlayerMenuKey&&playing&&!menu)showPlayerMenu(!playerMenu);
        if(p&&!wasPhotoKey&&playing&&!menu&&!playerMenu&&!photoResult)setPhotoMode(!photoMode);
        if(photoMode&&tab&&!wasPhotoCategory) {
            photoCategory=PhotoCategory((int(photoCategory)+1)%4);text("photo-category",categoryLabel(photoCategory));
        }
    }
    if(photoMode&&playerDown&&!photoResult)setPhotoMode(false);
    // Exposure happens after the camera has consumed this frame's mouse input.
    if(photoMode&&shutter&&!wasPhotoShutter&&!photoResult)photoShutterRequested=true;
    wasPlayerMenuKey=m;wasPhotoKey=p;wasPhotoCategory=tab;wasPhotoShutter=shutter;
}

void World::Impl::refreshPlayerMenu() {
    const auto cents=profile.wallet.balance();
    text("player-money",std::to_string(cents/100)+","+(cents%100<10?"0":"")+std::to_string(cents%100)+" €");
    text("player-save-status",profileError.empty()?"Progression sauvegardée sur cet ordinateur.":"Sauvegarde : "+profileError);
    for(const auto& tab:{"portfolio","inventory","ammo"}) {
        style(std::string("player-panel-")+tab,"display",playerTab==tab?"block":"none");
        if(auto* button=ui->findElementById(std::string("player-tab-")+tab)) {
            button->SetClass("selected",playerTab==tab);button->SetAttribute("aria-selected",playerTab==tab?"true":"false");ui->notifyJsMutation();
        }
    }
    text("player-items",profile.items.empty()?"Aucun objet.":"");
    std::string items;
    for(const auto& [id,n]:profile.items)items+=(id=="camera"?"Appareil photo numérique":id)+" × "+std::to_string(n)+"\n";
    text("player-items",items.empty()?"Aucun objet.":items);
    text("player-equipped",std::string("Arme équipée : ")+r1::combat::spec(arsenal.held()).name);
    text("player-ammo-pistol",std::to_string(arsenal.loaded(r1::combat::Weapon::Pistol))+" / 17 dans le chargeur · "+
        std::to_string(arsenal.reserve(r1::combat::Weapon::Pistol))+" en réserve");
    text("player-ammo-rocket",std::to_string(arsenal.loaded(r1::combat::Weapon::RocketLauncher))+" / 1 chargée · "+
        std::to_string(arsenal.reserve(r1::combat::Weapon::RocketLauncher))+" en réserve");
    refreshPortfolio();
}

void World::Impl::refreshPortfolio() {
    const char* filters[]={"all","world","animal","monument","event"};
    for(int i=0;i<5;++i)if(auto* button=ui->findElementById(std::string("portfolio-filter-")+filters[i])) {
        button->SetClass("selected",portfolioFilter==i-1);button->SetAttribute("aria-pressed",portfolioFilter==i-1?"true":"false");ui->notifyJsMutation();
    }
    std::map<std::string,std::string> species;
    for(const auto& photo:profile.photos)for(const auto& animal:photo.animals)species[animal.species]=animal.name;
    const auto escape=[](const std::string& value){
        std::string result;
        for(char c:value)switch(c){case '&':result+="&amp;";break;case '<':result+="&lt;";break;
            case '>':result+="&gt;";break;case '"':result+="&quot;";break;case '\'':result+="&#39;";break;default:result+=c;}
        return result;
    };
    std::string options="<option value=\"\">Toutes les espèces</option>";
    for(const auto& [id,name]:species)options+="<option value=\""+escape(id)+"\">"+escape(name)+"</option>";
    if(written["portfolio:species-options"]!=options&&ui->setElementRml("portfolio-species",options))written["portfolio:species-options"]=options;
    field("portfolio-species",portfolioSpecies);
    style("portfolio-species","display",portfolioFilter==1?"inline-block":"none");
    std::vector<size_t> indices;
    for(size_t i=0;i<profile.photos.size();++i)
        if((portfolioFilter<0||int(profile.photos[i].category)==portfolioFilter)&&hasPhotoSpecies(profile.photos[i],portfolioSpecies))indices.push_back(i);
    const size_t pages=(indices.size()+kPortfolioPageSize-1)/kPortfolioPageSize;
    portfolioIndex=indices.empty()?0:std::min(portfolioIndex/kPortfolioPageSize,pages-1)*kPortfolioPageSize;
    text("portfolio-count",std::to_string(indices.size())+(indices.size()==1?" photo":" photos")+" · Page "+
        (indices.empty()?"0":std::to_string(portfolioIndex/kPortfolioPageSize+1))+" / "+std::to_string(pages));
    for(auto id:{"portfolio-prev","portfolio-next"})if(auto* button=ui->findElementById(id)) {
        const bool disabled=indices.empty()||(std::string(id)=="portfolio-prev"?portfolioIndex==0:portfolioIndex+kPortfolioPageSize>=indices.size());
        if(disabled)button->SetAttribute("disabled","disabled");else button->RemoveAttribute("disabled");
        ui->notifyJsMutation();
    }
    text("portfolio-empty",indices.empty()?"Aucune photo dans cette catégorie. Reprenez l’exploration et appuyez sur P.":"");
    style("portfolio-empty","display",indices.empty()?"block":"none");
    for(size_t slot=0;slot<kPortfolioPageSize;++slot) {
        const auto suffix=std::to_string(slot),cardId="portfolio-card-"+suffix,imageId="portfolio-thumb-"+suffix;
        const Photograph* photo=portfolioIndex+slot<indices.size()?&profile.photos[indices[portfolioIndex+slot]]:nullptr;
        auto* card=ui->findElementById(cardId);auto* image=ui->findElementById(imageId);
        if(!card||!image)continue;
        style(cardId,"display",photo?"inline-block":"none");
        card->SetAttribute("data-available",photo?"true":"false");card->SetAttribute("data-slot",suffix);
        if(!photo){image->RemoveAttribute("src");continue;}
        std::error_code error;
        bool present=fs::is_regular_file(profileStore->photoPath(*photo),error);
        const auto source=present?fs::absolute(profileStore->photoPath(*photo)).generic_string():std::string();
        if(present&&image->GetAttribute<std::string>("src","")!=source)image->SetAttribute("src",source);
        Rml::Vector2f dimensions;float aspect=1.6f;
        if(present&&(!image->GetIntrinsicDimensions(dimensions,aspect)||!std::isfinite(aspect)||aspect<=0))present=false;
        if(!present){image->RemoveAttribute("src");aspect=1.6f;}
        const float width=std::min(281.f,176.f*aspect);
        image->SetProperty("width",number(width,2)+"px");image->SetProperty("height",number(width/aspect,2)+"px");
        style(imageId,"display",present?"block":"none");
        style("portfolio-missing-"+suffix,"display",present?"none":"block");
        text("portfolio-card-title-"+suffix,photo->title);
        std::string animals;
        for(const auto& a:photo->animals){if(!animals.empty())animals+=", ";animals+=a.name;}
        const auto details=std::string(categoryLabel(photo->category))+(animals.empty()?"":" · "+animals)+" · "+photo->takenUtc+"\n"+photo->place+" · "+
            number(photo->latitude,5)+", "+number(photo->longitude,5)+" · "+number(photo->altitude,1)+" m"+(present?"":"\nImage absente ou illisible.");
        card->SetAttribute("data-title",photo->title);card->SetAttribute("data-details",details);
        card->SetAttribute("data-aspect",number(aspect,6));card->SetAttribute("aria-label","Ouvrir : "+photo->title);
        ui->notifyJsMutation();
    }
}

bool World::Impl::playerMenuAction(const std::string& id) {
    if(id.rfind("player-",0)!=0&&id.rfind("portfolio-",0)!=0)return false;
    if(!playerMenu)return true;
    if(id=="portfolio-species") {
        portfolioSpecies=value(id);portfolioFilter=1;portfolioIndex=0;refreshPortfolio();
    }
    else if(id=="player-close")showPlayerMenu(false);
    else if(id=="player-map"){showPlayerMenu(false);showMap(true);}
    else if(id=="player-photo"){showPlayerMenu(false);setPhotoMode(true);}
    else if(id=="player-tab-portfolio"||id=="player-tab-inventory"||id=="player-tab-ammo") {
        ui->executeJS("closePortfolioViewer();");
        playerTab=id.substr(11);refreshPlayerMenu();
    } else if(id=="player-equip-fists"||id=="player-equip-pistol"||id=="player-equip-rocket") {
        selectWeapon(id=="player-equip-fists"?r1::combat::Weapon::Fists:id=="player-equip-pistol"?r1::combat::Weapon::Pistol:r1::combat::Weapon::RocketLauncher);
        savePlayerProfile();refreshPlayerMenu();
    } else if(id=="portfolio-prev"||id=="portfolio-next") {
        if(id=="portfolio-prev")portfolioIndex=portfolioIndex>=kPortfolioPageSize?portfolioIndex-kPortfolioPageSize:0;
        else portfolioIndex+=kPortfolioPageSize;
        refreshPortfolio();
    } else if(id.rfind("portfolio-filter-",0)==0) {
        portfolioFilter=id=="portfolio-filter-all"?-1:id=="portfolio-filter-world"?0:id=="portfolio-filter-animal"?1:id=="portfolio-filter-monument"?2:3;
        portfolioSpecies.clear();portfolioIndex=0;refreshPortfolio();
    }
    return true;
}

void World::Impl::runPhotoSmoke(double dt) {
    if(!playing||menu||drop.phase!=Drop::None)return;
    photoSmokeAge+=dt;
    auto fail=[&](const std::string& why){
        saida::Log::error("[World photo E2E] FAIL ",why);testFailed=true;
        if(const char* png=std::getenv("R1WORLD_PHOTO_FAILURE_SHOT")) {
            saida::CaptureRequest shot;shot.pngPath=png;shot.waitForAssets=false;engine.captureFrameThenExit(shot);
        } else engine.sceneTree().quit();
    };
    if(photoSmokePhase==0) {
        if(photoSmokeAge<3.||!engine.resources().assetLoadsSettled()||jumpOffset>.01)return;
        selectWeapon(r1::combat::Weapon::Pistol);
        if(!setPhotoMode(true)){fail("could not equip camera");return;}
        camera->fovDegrees=zoomPhotoFov(sceneFov,sceneFov,4);
        photoShutterRequested=true;photoSmokePhase=1;return;
    }
    if(photoSmokePhase==1||photoSmokePhase==2) {
        if(photoResult||photoShutterRequested)return;
        if(profile.photos.size()!=size_t(photoSmokePhase)||!profileError.empty()||!ui->enabled()) {
            fail("capture completion or saved portfolio: "+profileError);return;
        }
        if(!fs::is_regular_file(profileStore->photoPath(profile.photos.back()))){fail("PNG missing");return;}
        if(photoSmokePhase==1) {
            if(profile.photos.front().fovDegrees>=sceneFov){fail("zoom was not applied to the saved exposure");return;}
            if(std::getenv("R1WORLD_PHOTO_ANIMAL_SMOKE")&&profile.photos.front().animals.empty()){fail("live animal not classified");return;}
            photoCategory=PhotoCategory::Event;photoShutterRequested=true;photoSmokePhase=2;return;
        }
        if(arsenal.loaded(r1::combat::Weapon::Pistol)!=17||arsenal.loaded(r1::combat::Weapon::RocketLauncher)!=1) {
            fail("photo shutter consumed ammunition");return;
        }
        selectWeapon(r1::combat::Weapon::Fists);
        showPlayerMenu(true);photoSmokePhase=3;photoSmokeAge=0;return;
    }
    if(photoSmokeAge<.5)return;
    photoSmokeAge=0;
    if(photoSmokePhase==3) {
        if(!playerMenu||photoMode||minimapUi->enabled()){fail("player menu does not own input/presentation");return;}
        if(std::abs(camera->fovDegrees-sceneFov)>.001f){fail("photo zoom leaked into gameplay");return;}
        testClick("player-tab-inventory");
        if(playerTab!="inventory"){fail("inventory tab click");return;}
        photoSmokePhase=4;return;
    }
    if(photoSmokePhase==4) {
        testClick("player-equip-pistol");
        if(arsenal.held()!=r1::combat::Weapon::Pistol){fail("equip did not change the real arsenal");return;}
        testClick("player-tab-ammo");photoSmokePhase=5;return;
    }
    if(photoSmokePhase==5) {
        if(written["text:player-ammo-pistol"].find("17 / 17")==std::string::npos){fail("wrong ammunition count");return;}
        testClick("player-tab-portfolio");photoSmokePhase=6;return;
    }
    if(photoSmokePhase==6) {
        const auto category=profile.photos.back().category;
        const char* filters[]={"portfolio-filter-world","portfolio-filter-animal","portfolio-filter-monument","portfolio-filter-event"};
        testClick(filters[int(category)]);
        const auto categoryCount=std::count_if(profile.photos.begin(),profile.photos.end(),[&](const auto& p){return p.category==category;});
        const auto categoryLabel=std::to_string(categoryCount)+(categoryCount==1?" photo":" photos")+" · Page 1 / 1";
        if(portfolioFilter!=int(category)||written["text:portfolio-count"]!=categoryLabel){fail("portfolio filter");return;}
        std::string error;PlayerProfileStore reopened(game/"generated/photo-smoke");const auto saved=reopened.load(error);
        if(!error.empty()||saved.photos.size()!=2||saved.photos.back().category!=category||saved.arsenal!=arsenal.snapshot()||
           saved.photos.front().animals.size()!=profile.photos.front().animals.size()||saved.photos.front().fovDegrees!=profile.photos.front().fovDegrees) {
            fail("persistence round trip: "+error);return;
        }
        if(std::getenv("R1WORLD_PHOTO_ANIMAL_SMOKE")) {
            const auto species=profile.photos.front().animals.front().species;
            testClick("portfolio-filter-animal");field("portfolio-species",species);
            if(auto* select=ui->findElementById("portfolio-species"))select->DispatchEvent("change",{});
            const auto speciesCount=std::count_if(saved.photos.begin(),saved.photos.end(),[&](const auto& p){return hasPhotoSpecies(p,species);});
            const auto speciesLabel=std::to_string(speciesCount)+(speciesCount==1?" photo":" photos")+" · Page 1 / 1";
            if(portfolioSpecies!=species||!hasPhotoSpecies(saved.photos.front(),species)||written["text:portfolio-count"]!=speciesLabel) {
                fail("species classification/filter did not survive reload");return;
            }
            saida::Log::info("[World photo E2E] species category=",species," saved and selected");
        } else testClick("portfolio-filter-all");
        photoSmokePhase=7;return;
    }
    if(photoSmokePhase==7) {
        testClick("portfolio-card-0");photoSmokePhase=8;return;
    }
    if(photoSmokePhase==8) {
        auto* viewer=ui->findElementById("portfolio-viewer");
        if(!viewer||!viewer->IsClassSet("open")){fail("thumbnail click did not open the full-screen viewer");return;}
        testClick("portfolio-zoom-in");
        auto* viewport=ui->findElementById("portfolio-viewer-viewport");const auto at=viewport->GetAbsoluteOffset();
        const int x=int(at.x+720),y=int(at.y+350);
        ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Move,x,y,saida::WebCanvasNode::MouseButton::None);
        ui->fireScrollEvent(0,-2);
        if(std::stof(viewer->GetAttribute<std::string>("data-zoom","1"))<=1.25f){fail("viewer wheel zoom did not change the image");return;}
        ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Down,x,y,saida::WebCanvasNode::MouseButton::Left);
        ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Move,x+100,y+50,saida::WebCanvasNode::MouseButton::None);
        ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Up,x+100,y+50,saida::WebCanvasNode::MouseButton::Left);
        if(std::abs(std::stof(viewer->GetAttribute<std::string>("data-pan-x","0")))<1.f){fail("zoomed photo did not pan while dragging");return;}
        photoSmokePhase=9;return;
    }
    if(photoSmokePhase==9) {
        testClick("portfolio-zoom-reset");
        auto* viewer=ui->findElementById("portfolio-viewer");
        const auto zoom=viewer->GetAttribute<std::string>("data-zoom","");
        if(zoom!="1"){fail("photo fit reset: zoom remains "+zoom);return;}
        testClick("portfolio-viewer-close");
        if(viewer->IsClassSet("open")||!playerMenu){fail("cross did not return to the photo grid");return;}
        photoSmokePhase=10;return;
    }
    if(photoSmokePhase==10) {
        saida::Log::info("[World photo E2E] PASS captures, species filters, grid click, full-screen viewer, wheel zoom, drag, reset and cross-close");
        const char* shot=std::getenv("R1WORLD_PHOTO_MENU_SHOT");
        if(const char* enlarged=std::getenv("R1WORLD_PHOTO_VIEWER_SHOT")) {
            shot=enlarged;testClick("portfolio-card-0");ui->executeJS("zoomPortfolioPhoto(2);");
        }
        if(shot) {
            saida::CaptureRequest request;request.pngPath=shot;request.frame=4;
            engine.captureFrameThenExit(request);photoSmokePhase=11;
        } else engine.sceneTree().quit();
        return;
    }
}
} // namespace r1::world
