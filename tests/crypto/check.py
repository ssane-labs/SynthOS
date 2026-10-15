import os, subprocess, hashlib, hmac
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
from cryptography.hazmat.primitives.asymmetric import x25519, ec, rsa, padding, utils
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
import sys
CT=sys.argv[1]
def run(*a): return subprocess.run([CT]+list(a),capture_output=True,text=True).stdout.split()
fails=0
def check(name, got, want):
    global fails
    if got!=want: fails+=1; print("FAIL",name, got[:80], want[:80])
H={'sha256':hashes.SHA256,'sha384':hashes.SHA384,'sha512':hashes.SHA512}
for n in [0,1,55,56,111,112,127,128,129,1000]:
    d=os.urandom(n)
    for h in ['sha256','sha384','sha512']:
        check('hash'+h+str(n), run('hash',h,d.hex() or '00'[:0] if n else '')[0] if n else run('hash',h,'')[0], hashlib.new(h,d).hexdigest())
for n in [0,1,55,56,63,64,65,119,120,1000]:
    d=os.urandom(n)
    check('sha1-'+str(n), run('sha1',d.hex())[0] if n else run('sha1','')[0], hashlib.sha1(d).hexdigest())
for h in ['sha256','sha384']:
    salt,ikm,info=os.urandom(13),os.urandom(40),os.urandom(9)
    want=HKDF(H[h](),77,salt,info).derive(ikm).hex()
    check('hkdf'+h, run('hkdf',h,salt.hex(),ikm.hex(),info.hex(),'77')[0], want)
for kl in [16,32]:
    for pl in [0,1,15,16,17,100,1000]:
        k,iv,aad,pt=os.urandom(kl),os.urandom(12),os.urandom(13),os.urandom(pl)
        r=run('gcm',k.hex(),iv.hex(),aad.hex(),pt.hex())
        check('gcm%d/%d'%(kl,pl), r, [AESGCM(k).encrypt(iv,pt,aad).hex(),'open-ok'])
for pl in [0,1,63,64,65,300]:
    k,iv,aad,pt=os.urandom(32),os.urandom(12),os.urandom(7),os.urandom(pl)
    check('chacha%d'%pl, run('chacha',k.hex(),iv.hex(),aad.hex(),pt.hex()), [ChaCha20Poly1305(k).encrypt(iv,pt,aad).hex(),'open-ok'])
for i in range(5):
    a=x25519.X25519PrivateKey.generate(); b=x25519.X25519PrivateKey.generate()
    ap=a.private_bytes(serialization.Encoding.Raw,serialization.PrivateFormat.Raw,serialization.NoEncryption())
    bp=b.public_key().public_bytes(serialization.Encoding.Raw,serialization.PublicFormat.Raw)
    check('x25519',run('x25519',ap.hex(),bp.hex())[0], a.exchange(b.public_key()).hex())
for cv,curve,h in [(0,ec.SECP256R1(),'sha256'),(1,ec.SECP384R1(),'sha384'),(0,ec.SECP256R1(),'sha384'),(1,ec.SECP384R1(),'sha256')]:
    for i in range(3):
        k=ec.generate_private_key(curve); size=curve.key_size//8
        sec=k.private_numbers().private_value.to_bytes(size,'big')
        pub=k.public_key().public_bytes(serialization.Encoding.X962,serialization.PublicFormat.UncompressedPoint)
        check('ecpub%d'%cv, run('ecpub',str(cv),sec.hex())[0], pub.hex())
        o=ec.generate_private_key(curve); opub=o.public_key().public_bytes(serialization.Encoding.X962,serialization.PublicFormat.UncompressedPoint)
        check('ecdh%d'%cv, run('ecdh',str(cv),sec.hex(),opub.hex())[0], k.exchange(ec.ECDH(),o.public_key()).hex())
        msg=os.urandom(50); dig=hashlib.new(h,msg).digest()
        sig=k.sign(dig, ec.ECDSA(utils.Prehashed(H[h]())))
        r,s=utils.decode_dss_signature(sig)
        rb=r.to_bytes((r.bit_length()+7)//8,'big'); sb=s.to_bytes((s.bit_length()+7)//8,'big')
        check('ecdsa%d'%cv, run('ecdsa',str(cv),pub.hex(),dig.hex(),rb.hex(),sb.hex())[0], 'valid')
        bad=bytearray(dig); bad[0]^=1
        check('ecdsa-bad%d'%cv, run('ecdsa',str(cv),pub.hex(),bytes(bad).hex(),rb.hex(),sb.hex())[0], 'invalid')
for bits in [2048,3072,4096]:
    k=rsa.generate_private_key(65537,bits); pn=k.public_key().public_numbers()
    n=pn.n.to_bytes((bits+7)//8,'big'); e=pn.e.to_bytes(3,'big')
    for h in ['sha256','sha384','sha512']:
        msg=os.urandom(30); dig=hashlib.new(h,msg).digest()
        s1=k.sign(msg,padding.PKCS1v15(),H[h]())
        check('rsa%d%s'%(bits,h), run('rsa',n.hex(),e.hex(),h,dig.hex(),s1.hex())[0],'valid')
        s2=k.sign(msg,padding.PSS(padding.MGF1(H[h]()),H[h]().digest_size),H[h]())
        check('pss%d%s'%(bits,h), run('pss',n.hex(),e.hex(),h,dig.hex(),s2.hex())[0],'valid')
        bad=bytearray(dig); bad[3]^=4
        check('pssbad', run('pss',n.hex(),e.hex(),h,bytes(bad).hex(),s2.hex())[0],'invalid')
print("crypto tests: %d failures" % fails)
sys.exit(1 if fails else 0)
