"""Build a local, provenance-labelled texture audition gallery; never approves a pack.

Run select in hd_assets.py first, then prepare, generate (exclusive GPU use), render.
The 2x neural candidates are reduced from the same 4x colour inference; alpha is
always resampled separately from the original. All media stays outside the repo.
"""
import argparse
import json
import shutil
from pathlib import Path
from types import SimpleNamespace

import hd_assets as hd

MODELS = ("lanczos", "realesrnet-x4plus", "realesrgan-x4plus")


def group_key(asset):
    return ("bleed" if asset["bleed"] else "plain") + "-" + asset["alpha"]


def prepare(root, assets):
    expected = {key: {a["source_id"]+".png" for a in assets if group_key(a)==key}
                for key in {group_key(a) for a in assets}}
    for key, names in expected.items():
        target = (root / "inputs" / key).resolve()
        if not target.is_relative_to(root.resolve()):
            raise ValueError("Input group escapes review root")
        if target.exists():
            for path in target.glob("*.png"):
                if path.name not in names:
                    path.unlink()  # Only stale generated group inputs, never source assets.
    for asset in assets:
        target = root / "inputs" / group_key(asset)
        target.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / asset["original"], target / (asset["source_id"] + ".png"))


def generate(root, assets, tool):
    for model in MODELS:
        for key in sorted({group_key(a) for a in assets}):
            first = next(a for a in assets if group_key(a) == key)
            print("GROUP", model, key, flush=True)
            hd.upscale(SimpleNamespace(input=root / "inputs" / key,
                output=root / "jobs" / model / key, tool=tool if model != "lanczos" else None,
                model=model, scale=4, padding=8, limit=100, bleed=first["bleed"],
                alpha=first["alpha"], wrap_u=False, wrap_v=False))


def checker(size, colour="checker"):
    from PIL import Image, ImageDraw
    out = Image.new("RGB", size, (35, 37, 43) if colour == "checker" else colour)
    if colour == "checker":
        draw = ImageDraw.Draw(out)
        for y in range(0, size[1], 12):
            for x in range(0, size[0], 12):
                if (x // 12 + y // 12) % 2:
                    draw.rectangle((x, y, x+11, y+11), fill=(52, 54, 60))
    return out


def render(root, assets):
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 18)
    small = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 15)
    preview = root / "previews"; preview.mkdir(exist_ok=True)
    for asset in assets:
        ident = asset["source_id"]
        original = Image.open(root / asset["original"]).convert("RGBA")
        asset["variants"] = {}
        for model in MODELS:
            source = root / "jobs" / model / group_key(asset) / "textures" / (ident + ".png")
            image = Image.open(source).convert("RGBA")
            if image.size != (original.width*4, original.height*4):
                raise ValueError("Invalid candidate dimensions: " + str(source))
            for scale in (2, 4):
                destination = root / "variants" / model / str(scale) / (ident + ".png")
                destination.parent.mkdir(parents=True, exist_ok=True)
                if scale == 4:
                    shutil.copyfile(source, destination)
                else:
                    # Resample colour without premultiplied-alpha contamination.
                    colour = image.convert("RGB").resize((original.width*2, original.height*2), Image.Resampling.LANCZOS)
                    alpha = hd.padded(original.getchannel("A"), 8)
                    alpha = alpha.resize((alpha.width*2, alpha.height*2),
                        Image.Resampling.NEAREST if asset["alpha"] == "nearest" else Image.Resampling.LANCZOS)
                    colour.putalpha(alpha.crop((16,16,alpha.width-16,alpha.height-16)))
                    colour.save(destination)
                asset["variants"][model + ":" + str(scale)] = destination.relative_to(root).as_posix()
        thumb = Image.open(root / asset["variants"]["realesrnet-x4plus:4"]).convert("RGBA"); thumb.thumbnail((220,160), Image.Resampling.LANCZOS)
        bg = checker((220,160)); bg.paste(thumb, ((220-thumb.width)//2,(160-thumb.height)//2), thumb)
        bg.save(preview / (asset["sample"] + ".jpg"), quality=92)
        asset["preview"] = "previews/" + asset["sample"] + ".jpg"
        asset["channels"] = {}
        for key, path in {"original": asset["original"], **asset["variants"]}.items():
            # Browser canvas discards RGB under zero alpha. Supply RGB-only files
            # explicitly so the viewer can really inspect colour bleeding there.
            rgba = Image.open(root / path).convert("RGBA")
            for channel in ("rgb", "alpha"):
                if channel == "rgb" and asset["alpha_extrema"] == [255,255]:
                    asset["channels"][key + ":" + channel] = path
                    continue  # Opaque originals already expose all colour data.
                target = root / "channels" / channel / (path.replace("/", "_").replace(":", "_"))
                target.parent.mkdir(parents=True, exist_ok=True)
                (rgba.convert("RGB") if channel == "rgb" else rgba.getchannel("A")).save(target)
                asset["channels"][key + ":" + channel] = target.relative_to(root).as_posix()
    # Eight readable comparison sheets, rather than one enormous image.
    sheets = root / "sheets"; sheets.mkdir(exist_ok=True)
    for page in range((len(assets)+4)//5):
        sheet = Image.new("RGB", (1440,1640), (24,26,31)); draw = ImageDraw.Draw(sheet)
        for c, label in enumerate(("Original (nearest)", "Lanczos 4x", "RealESRNet 4x", "RealESRGAN 4x")):
            draw.text((c*360+12,12), label, fill="white", font=font)
        for row, asset in enumerate(assets[page*5:page*5+5]):
            y = 48 + row*316
            draw.text((12,y), asset["sample"]+" | "+asset["category"]+" | "+asset["label"], fill="white", font=font)
            draw.text((12,y+24), str(asset["width"])+" x "+str(asset["height"])+" | alpha: "+asset["alpha"]+" | RGB bleed: "+str(asset["bleed"]), fill="#aab6c6", font=small)
            paths = [asset["original"]] + [asset["variants"][m+":4"] for m in MODELS]
            for col, path in enumerate(paths):
                image = Image.open(root/path).convert("RGBA")
                scale = min(336/image.width, 250/image.height)
                image = image.resize((max(1,round(image.width*scale)),max(1,round(image.height*scale))),
                    Image.Resampling.NEAREST if col == 0 else Image.Resampling.LANCZOS)
                bg = checker((336,250)); bg.paste(image, ((336-image.width)//2,(250-image.height)//2), image)
                sheet.paste(bg, (col*360+12,y+50))
        sheet.save(sheets / ("comparison-"+str(page+1).zfill(2)+".jpg"), quality=95)
    overview = Image.new("RGB",(1200,1840),(24,26,31)); draw=ImageDraw.Draw(overview)
    for i, asset in enumerate(assets):
        x=(i%5)*240; y=(i//5)*230
        overview.paste(Image.open(root/asset["preview"]),(x+10,y+8))
        draw.text((x+10,y+174),asset["sample"]+" "+asset["category"],fill="white",font=font)
        draw.text((x+10,y+200),asset["label"][:27],fill="#aab6c6",font=small)
    overview.save(root / "overview.jpg",quality=94)
    recipes = {model+":"+key: json.loads((root/"jobs"/model/key/"build.json").read_text())["recipe"]
               for model in MODELS for key in sorted({group_key(a) for a in assets})}
    fingerprint = dict(originals=[(a["source_id"],a["original_hash"]) for a in assets], recipes=recipes,
                       review_runner=hd.digest(Path(__file__).read_bytes()))
    metadata = dict(schema=1, assets=assets, recipes=recipes,
                    review_id=hd.digest(json.dumps(fingerprint,sort_keys=True).encode()),
                    note="Review only: offline SHPX identifiers must be correlated to runtime dumps before pack installation. 2x colour is downsampled from 4x; alpha is separate. No face restoration. Wrap axes remain unverified (clamp audition).")
    (root/"review.json").write_text(json.dumps(metadata,indent=2),encoding="utf-8")
    # JSON escapes prevent archive labels from injecting script markup.
    payload=json.dumps(metadata).replace("<","\\u003c").replace(">","\\u003e").replace("&","\\u0026")
    (root/"review.html").write_text(HTML.replace("__DATA__",payload),encoding="utf-8")
    print("READY",len(assets),"assets; 240 candidates;",root/"review.html",flush=True)


HTML = r'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Def Jam FFNY — 40 texture auditions</title>
<style>*{box-sizing:border-box}body{margin:0;background:#171b23;color:#edf2fa;font:15px system-ui}header,main{padding:20px}h1{font-size:25px;margin:0 0 8px}p{color:#bac6d7;line-height:1.5}select,button,input,textarea{background:#263143;color:#fff;border:1px solid #52637a;border-radius:6px;padding:8px}button{cursor:pointer}a{color:#92c9ff}nav{display:flex;gap:10px;flex-wrap:wrap;align-items:center}.panels{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;margin:16px 0}.panel{min-width:0;background:#232b37;border-radius:8px;overflow:hidden}.panel h3{margin:10px;font-size:15px}.panel a{display:block;margin:8px;font-size:13px}canvas{width:100%;height:390px;display:block;cursor:grab;touch-action:none}.meta{overflow-wrap:anywhere;color:#bac6d7;font-size:13px}#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:12px}#grid button{text-align:left;padding:8px}#grid img{width:100%;height:160px;object-fit:contain}#grid span{display:block;margin:5px 0}.active{outline:3px solid #85bdff}textarea{width:min(700px,100%);height:70px;display:block;margin:10px 0}details{margin:18px 0}#status{color:#93d9b9}@media(max-width:900px){.panels{grid-template-columns:repeat(2,minmax(0,1fr))}canvas{height:300px}}</style>
<header><h1>Def Jam FFNY · 40 representative texture auditions</h1><p>Compare the original with Lanczos, RealESRNet and RealESRGAN. Click an asset below. Drag any pane to pan all four; use zoom to inspect faces, lettering and edges. No face restoration.</p><nav><label>Category <select id="category"></select></label><button id="prev">← Previous</button><select id="asset"></select><button id="next">Next →</button><label>Scale <select id="scale"><option value="4">4×</option><option value="2">2× (from 4× colour)</option></select></label><label>Zoom <select id="zoom"><option value="fit">Fit</option><option value="1">100% output pixels</option><option value="2">200%</option></select></label><label>View <select id="channel"><option value="rgba">Colour + alpha</option><option value="rgb">RGB only</option><option value="alpha">Alpha only</option></select></label><label>Background <select id="background"><option>checker</option><option>black</option><option>white</option></select></label><button id="reset">Centre</button></nav></header>
<main><h2 id="title"></h2><div class="meta" id="meta"></div><div class="panels" id="panels"></div><span id="status"></span><p id="note"></p><label>Review choice <select id="choice"><option value="">Undecided</option><option value="original">Keep original</option><option value="lanczos">Lanczos</option><option value="realesrnet-x4plus">RealESRNet</option><option value="realesrgan-x4plus">RealESRGAN</option><option value="hold">Needs another approach</option></select></label><textarea id="notes" placeholder="Optional notes: face identity, lettering, seams, masks…"></textarea><button id="export">Export review choices (.json)</button><p>Choices are saved in this browser. Export them to share your decisions. These samples are for review; in-game acceptance and wrap-axis validation remain pending.</p><details><summary>Recipe and limitations</summary><p id="recipe"></p><p>RGB and alpha are separate. Eight source pixels of edge padding are cropped after inference. UI/logo RGB is spread into fully transparent pixels; world RGB is left intact. Effects use nearest-neighbour alpha to preserve mask values. Other alpha uses Lanczos. The original is shown with nearest-neighbour enlargement, without generated detail. All panes use identical positioning. Fit view reduces large images, so inspect at 100% for detail. The 2× candidates use the same 4× colour output with Lanczos reduction; alpha is resampled directly from the original.</p><p>Font vectorisation and movies are follow-up workflows, outside this static-texture audition. No asset is automatically approved by this gallery.</p><p>Printable comparison sheets: <a href="sheets/comparison-01.jpg">1</a> · <a href="sheets/comparison-02.jpg">2</a> · <a href="sheets/comparison-03.jpg">3</a> · <a href="sheets/comparison-04.jpg">4</a> · <a href="sheets/comparison-05.jpg">5</a> · <a href="sheets/comparison-06.jpg">6</a> · <a href="sheets/comparison-07.jpg">7</a> · <a href="sheets/comparison-08.jpg">8</a></p></details><h2>All 40 assets</h2><p>Thumbnails show RealESRNet at 4× for navigation. Compare the full candidates above before choosing.</p><div id="grid"></div></main>
<script>const data=__DATA__, assets=data.assets, models=['original','lanczos','realesrnet-x4plus','realesrgan-x4plus'], names=['Original · nearest','Lanczos','RealESRNet','RealESRGAN'];const $=id=>document.getElementById(id);let selected=0,images=[],token=0,pan=[0,0],reviews={};try{reviews=JSON.parse(localStorage.getItem('defjam-hd-review-v3:'+data.review_id)||'{}')}catch(e){}const canvases=[],links=[];models.forEach((m,i)=>{const p=document.createElement('section');p.className='panel';const h=document.createElement('h3');h.textContent=names[i];p.append(h);const c=document.createElement('canvas');p.append(c);canvases.push(c);const a=document.createElement('a');a.target='_blank';a.textContent='Open full PNG';p.append(a);links.push(a);$('panels').append(p)});$('recipe').textContent=data.note;
const cats=['All',...new Set(assets.map(a=>a.category))];cats.forEach(c=>$('category').add(new Option(c,c)));assets.forEach((a,i)=>{$('asset').add(new Option(a.sample+' · '+a.label,i));const b=document.createElement('button');b.dataset.index=i;const im=document.createElement('img');im.loading='lazy';im.src=a.preview;im.alt=a.label;b.append(im);for(const t of [a.sample+' · '+a.category,a.label]){const s=document.createElement('span');s.textContent=t;b.append(s)}b.onclick=()=>pick(i);$('grid').append(b)});
function reviewKey(){return assets[selected].sample+':'+$('scale').value}function filtered(){return assets.map((a,i)=>i).filter(i=>$('category').value==='All'||assets[i].category===$('category').value)}function pick(i){selected=+i;pan=[0,0];$('asset').value=selected;load()}
function draw(){if(images.length!==4)return;const a=assets[selected],scale=+$('scale').value,w=a.width*scale,h=a.height*scale;$('status').textContent='Loaded · target '+w+'×'+h+' · '+$('zoom').selectedOptions[0].text;canvases.forEach((c,i)=>{const rect=c.getBoundingClientRect();c.width=Math.round(rect.width);c.height=Math.round(rect.height);const ctx=c.getContext('2d');ctx.fillStyle=$('background').value==='white'?'#fff':$('background').value==='black'?'#000':'#23272e';ctx.fillRect(0,0,c.width,c.height);if($('background').value==='checker'){ctx.fillStyle='#353b45';for(let y=0;y<c.height;y+=16)for(let x=0;x<c.width;x+=16)if((x/16+y/16)%2)ctx.fillRect(x,y,16,16)}const z=$('zoom').value==='fit'?Math.min((c.width-16)/w,(c.height-16)/h):+$('zoom').value;ctx.imageSmoothingEnabled=i!==0&&z<1;ctx.drawImage(images[i],(c.width-w*z)/2+pan[0],(c.height-h*z)/2+pan[1],w*z,h*z)})}
async function load(){const t=++token,a=assets[selected];$('title').textContent=a.sample+' · '+a.label;$('meta').textContent=a.category+' | original '+a.width+'×'+a.height+' | SHPX type 0x'+a.type.toString(16)+' | alpha '+a.alpha_extrema.join('–')+' | '+a.source+' [image '+a.index+']';$('note').textContent=a.note;$('choice').value=reviews[reviewKey()]?.choice||'';$('notes').value=reviews[reviewKey()]?.notes||'';images=[];$('choice').disabled=$('notes').disabled=true;canvases.forEach(c=>{const ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height)});links.forEach(l=>l.removeAttribute('href'));document.querySelectorAll('#grid button').forEach(b=>b.classList.toggle('active',+b.dataset.index===selected));$('status').textContent='Loading four full-resolution candidates…';const keys=['original',...models.slice(1).map(m=>m+':'+$('scale').value)];const paths=keys.map(k=>$('channel').value==='rgba'?(k==='original'?a.original:a.variants[k]):a.channels[k+':'+$('channel').value]);try{const loaded=await Promise.all(paths.map(p=>new Promise((resolve,reject)=>{const im=new Image;im.onload=()=>resolve(im);im.onerror=()=>reject(new Error(p));im.src=p})));if(t!==token)return;images=loaded;links.forEach((l,i)=>l.href=paths[i]);$('choice').disabled=$('notes').disabled=false;$('status').textContent='Loaded · target '+a.width*+$('scale').value+'×'+a.height*+$('scale').value+' · '+$('zoom').selectedOptions[0].text;draw()}catch(e){if(t!==token)return;$('status').textContent='Image failed to load: '+e.message}}
$('asset').onchange=()=>pick($('asset').value);$('category').onchange=()=>{document.querySelectorAll('#grid button').forEach(b=>b.hidden=$('category').value!=='All'&&assets[+b.dataset.index].category!==$('category').value);for(const o of $('asset').options)o.hidden=$('category').value!=='All'&&assets[+o.value].category!==$('category').value;pick(filtered()[0])};$('prev').onclick=()=>{const f=filtered();pick(f[(f.indexOf(selected)+f.length-1)%f.length])};$('next').onclick=()=>{const f=filtered();pick(f[(f.indexOf(selected)+1)%f.length])};for(const id of ['scale','channel'])$(id).onchange=()=>{pan=[0,0];load()};for(const id of ['zoom','background'])$(id).onchange=draw;$('reset').onclick=()=>{pan=[0,0];draw()};canvases.forEach(c=>{let last=null;c.onpointerdown=e=>{last=[e.clientX,e.clientY];c.setPointerCapture(e.pointerId)};c.onpointermove=e=>{if(!last)return;pan[0]+=e.clientX-last[0];pan[1]+=e.clientY-last[1];last=[e.clientX,e.clientY];draw()};c.onpointerup=c.onpointercancel=()=>last=null});function save(){const a=assets[selected];reviews[reviewKey()]={sample:a.sample,source:a.source,index:a.index,source_id:a.source_id,choice:$('choice').value,scale:+$('scale').value,notes:$('notes').value};try{localStorage.setItem('defjam-hd-review-v3:'+data.review_id,JSON.stringify(reviews))}catch(e){$('status').textContent='Browser storage unavailable; export choices before closing.'}}$('choice').onchange=$('notes').oninput=save;$('export').onclick=()=>{const blob=new Blob([JSON.stringify({schema:1,review_only:true,review_id:data.review_id,choices:Object.fromEntries(Object.entries(reviews).filter(([key,r])=>assets.some(a=>a.sample===r.sample&&a.source_id===r.source_id)&&[2,4].includes(r.scale)))},null,2)],{type:'application/json'});const a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='defjam-hd-review-choices.json';a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)};window.onresize=draw;load();</script></html>'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action",choices=("prepare","generate","render"))
    parser.add_argument("--root",required=True)
    parser.add_argument("--tool")
    args=parser.parse_args(); root=Path(args.root)
    assets=json.loads((root/"selection.json").read_text(encoding="utf-8"))["assets"]
    if args.action=="prepare": prepare(root,assets)
    elif args.action=="generate": generate(root,assets,args.tool)
    else: render(root,assets)


if __name__=="__main__": main()
