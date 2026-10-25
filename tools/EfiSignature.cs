// SPDX-License-Identifier: MIT
// Narrow verifier for this PE32+ EFI layout; not a general PE/Authenticode parser.
using System;
using System.IO;
using System.Text;
using System.Security.Cryptography;
using System.Security.Cryptography.Pkcs;
namespace GpuUnlock {
    public sealed class SignatureResult {
        public string Thumbprint;
        public string ImageDigestSha256;
        public string ContentType;
    }
    public static class EfiSignature {
        static void Need(bool ok, string why) { if (!ok) throw new InvalidDataException(why); }
        static int U32(byte[] b, int off) {
            Need(off >= 0 && off <= b.Length-4, "Truncated integer");
            uint n=BitConverter.ToUInt32(b,off); Need(n<=Int32.MaxValue,"Integer too large"); return (int)n;
        }
        static byte[] Slice(byte[] b,int off,int length) {
            Need(off>=0&&length>=0&&off<=b.Length-length,"Invalid range");
            byte[] r=new byte[length]; Buffer.BlockCopy(b,off,r,0,length); return r;
        }
        static bool Equal(byte[] a,byte[] b) {
            if(a.Length!=b.Length)return false;
            for(int i=0;i<a.Length;i++)if(a[i]!=b[i])return false; return true;
        }
        static byte[] Der(byte[] b,ref int pos,byte tag) {
            Need(pos>=0&&pos<=b.Length-2&&b[pos++]==tag,"Unexpected DER tag");
            int n=b[pos++];
            if((n&128)!=0) {
                int count=n&127; Need(count>0&&count<=4&&pos<=b.Length-count&&b[pos]!=0,"Invalid DER length");
                long size=0; for(int i=0;i<count;i++)size=(size<<8)|b[pos++];
                Need(size>=128&&size<=Int32.MaxValue,"Invalid DER size"); n=(int)size;
            }
            byte[] value=Slice(b,pos,n); pos+=n; return value;
        }
        static int Header(byte[] b) {
            Need(b.Length>=64&&b[0]=='M'&&b[1]=='Z',"Not a PE");
            int pe=U32(b,60);
            Need(pe<=b.Length-264&&b[pe]=='P'&&b[pe+1]=='E'&&b[pe+2]==0&&b[pe+3]==0,"Bad PE header");
            Need(BitConverter.ToUInt16(b,pe+4)==0x8664,"Not AMD64");
            int opt=pe+24;
            Need(BitConverter.ToUInt16(b,opt)==0x20b&&BitConverter.ToUInt16(b,opt+68)==10,"Not PE32+ EFI application");
            Need(U32(b,opt+108)>=5&&BitConverter.ToUInt16(b,pe+20)>=240,"Missing security directory");
            return opt;
        }
        public static void Unchanged(byte[] original, byte[] signed) {
            int opt=Header(original); Need(Header(signed)==opt,"PE header moved");
            int security=opt+144;
            Need(U32(original,security)==0&&U32(original,security+4)==0,"Original already signed");
            Need(U32(signed,security)==original.Length&&signed.Length>original.Length,"Signature must be appended to exact original");
            for(int i=0;i<original.Length;i++) {
                if((i>=opt+64&&i<opt+68)||(i>=security&&i<security+8))continue;
                Need(original[i]==signed[i],"Signed image changed executable bytes at "+i);
            }
        }
        public static SignatureResult Verify(byte[] image,string expectedThumbprint) {
            int opt=Header(image), security=opt+144, cert=U32(image,security), size=U32(image,security+4);
            Need(cert>0&&cert%8==0&&size>=8&&cert<=image.Length-size&&cert+size==image.Length,"Bad certificate table");
            int headerSize=U32(image,opt+60), pe=opt-24;
            int count=BitConverter.ToUInt16(image,pe+6), table=opt+BitConverter.ToUInt16(image,pe+20);
            Need(count>0&&count<=32&&headerSize<=cert&&table+count*40<=headerSize,"Bad section table");
            int cursor=headerSize;
            var ranges=new System.Collections.Generic.SortedDictionary<int,int>();
            for(int i=0;i<count;i++) {
                int rawSize=U32(image,table+i*40+16), raw=U32(image,table+i*40+20);
                if(rawSize>0) {Need(!ranges.ContainsKey(raw),"Duplicate section");ranges.Add(raw,rawSize);}
            }
            foreach(var r in ranges) {
                Need(r.Key==cursor&&r.Value<=cert-cursor,"Non-contiguous or invalid raw sections");
                cursor+=r.Value;
            }
            Need(cursor==cert,"Overlay/gap not supported by narrow verifier");
            int length=U32(image,cert);
            Need(length>=8&&length<=size&&size-length<8&&BitConverter.ToUInt16(image,cert+4)==0x200&&BitConverter.ToUInt16(image,cert+6)==2,"Invalid WIN_CERTIFICATE");
            byte[] padded=Slice(image,cert+8,length-8); int pos=0;
            Der(padded,ref pos,0x30);
            for(int i=pos;i<padded.Length;i++)Need(padded[i]==0,"Unexpected certificate suffix");
            for(int i=length;i<size;i++)Need(image[cert+i]==0,"Unexpected table padding");
            var cms=new SignedCms(); cms.Decode(Slice(padded,0,pos));
            Need(cms.ContentInfo.ContentType.Value=="1.3.6.1.4.1.311.2.1.4","Not Authenticode content");
            Need(cms.SignerInfos.Count==1&&cms.SignerInfos[0].Certificate!=null,"Expected one signer");
            cms.CheckSignature(true); // Cryptographic check only; intentionally does not confer db trust.
            var signer=cms.SignerInfos[0];
            Need(signer.DigestAlgorithm.Value=="2.16.840.1.101.3.4.2.1","Expected SHA256 signature");
            if(!String.IsNullOrEmpty(expectedThumbprint))
                Need(String.Equals(signer.Certificate.Thumbprint,expectedThumbprint,StringComparison.OrdinalIgnoreCase),"Wrong signer");
            byte[] content=cms.ContentInfo.Content; pos=0;
            byte[] indirect=Der(content,ref pos,0x30); Need(pos==content.Length,"Trailing signed content");
            pos=0; Der(indirect,ref pos,0x30); byte[] info=Der(indirect,ref pos,0x30); Need(pos==indirect.Length,"Trailing digest info");
            pos=0; byte[] algorithm=Der(info,ref pos,0x30); byte[] wanted=Der(info,ref pos,0x04); Need(pos==info.Length&&wanted.Length==32,"Invalid SHA256 digest");
            int algPos=0; byte[] oid=Der(algorithm,ref algPos,0x06);
            Need(Equal(oid,new byte[]{0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01}),"Wrong image digest algorithm");
            if(algPos<algorithm.Length)Need(Der(algorithm,ref algPos,0x05).Length==0,"Invalid algorithm parameters");
            Need(algPos==algorithm.Length,"Trailing algorithm data");
            byte[] digest;
            using(var stream=new MemoryStream()) {
                stream.Write(image,0,opt+64);
                stream.Write(image,opt+68,security-(opt+68));
                stream.Write(image,security+8,cert-(security+8));
                using(var sha=SHA256.Create())digest=sha.ComputeHash(stream.ToArray());
            }
            Need(Equal(digest,wanted),"Authenticode image digest mismatch");
            return new SignatureResult { Thumbprint=signer.Certificate.Thumbprint,
                ImageDigestSha256=BitConverter.ToString(digest).Replace("-","").ToLowerInvariant(),
                ContentType=cms.ContentInfo.ContentType.Value };
        }
    }
}
